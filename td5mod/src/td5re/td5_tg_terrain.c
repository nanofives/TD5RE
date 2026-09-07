/**
 * td5_tg_terrain.c -- auto-track TERRAIN + BIOMES: trees, props, road surfaces, biome table + adjacency, snow, sea, outskirts, topo authority, forest side roads, tree bands, ponds, ground chain, gantry/sign context
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

const TG_TreePage k_tree_pages[TD5_TG_TREE_VARIANTS] = {
    { 4200, 5600, TG_TREE_DECID,   0 },  /* 0  L017 p266  deciduous         */
    { 5200, 6600, TG_TREE_DECID,   0 },  /* 1  L008 p173  big deciduous     */
    { 4000, 5400, TG_TREE_DECID,   1 },  /* 2  L013 p234  deciduous, half   */
    { 3200, 6400, TG_TREE_CONIFER, 1 },  /* 3  L003 p441  conifer, half     */
    { 3200, 6400, TG_TREE_CONIFER, 1 },  /* 4  L003 p445  snow conifer, half*/
    { 3400, 7200, TG_TREE_PALM,    0 },  /* 5  L026 p097  palm              */
    { 3800, 6800, TG_TREE_PALM,    0 },  /* 6  L014 p249  palm              */
    { 3600, 4200, TG_TREE_TOPIARY, 0 },  /* 7  L004 p359  topiary           */
    { 3600, 5000, TG_TREE_TOPIARY, 0 },  /* 8  L004 p360  topiary           */
    { 4200, 6000, TG_TREE_WILLOW,  1 }   /* 9  L004 p369  willow, half      */
};

/* TEXTURES.DAT page slot for tree variant v (variant 0 reuses PAGE_TREE). */
int tg_tree_slot(int v)
{
    return v == 0 ? TD5_TG_PAGE_TREE : (TD5_TG_PAGE_TREE_EXTRA + v - 1);
}

/* [R5 item 18] Page slot for the v-th tall Moscow park tree, in the reserved
 * FLORA block. Only the first k_r5_flora_tree_count slots are ever filled. */
int tg_flora_tree_slot(int v)
{
    return TD5_TG_PAGE_R5_FLORA + v;
}

/* [R7 item 8] Is FLORA page `v` a horizontally CONTINUOUS BAND rather than a
 * single-tree silhouette? A grove backdrop / treeline page is authored to TILE
 * edge-to-edge, so standing one upright as a camera-facing billboard shows a
 * SLICE of the band -- "the texture is cut off because it can be continuous".
 *
 * R6 fixed exactly this at span 462 but by a HAND-PICKED list { 0, 2 }: it
 * excluded FLORA index 1 (p412, the dense green grove wall) and KEPT index 2
 * (p424, the autumn/orange canopy). Index 2 has the SAME property -- it too
 * bleeds foliage off both edges -- so the user now reports the orange tree at the
 * same span. Listing pages fixes the instance; measuring the property fixes the
 * class (and any future page).
 *
 * The tell is in the alpha key (index 0 = transparent): a single tree leaves
 * transparent SKY down both sides, so its opaque pixels never reach both image
 * edges in the same row; a tiling band bleeds opaque foliage off BOTH edges so
 * neighbouring copies join. Measured over the shipped pages (64x64), the
 * both-edge-touch fraction is 0.00 for the individual canopy (FLORA0) and
 * 0.77 / 0.86 for the two bands (FLORA1 grove wall, FLORA2 autumn) -- a wide gap
 * the 0.40 threshold sits inside. */
static int tg_flora_page_is_band(int v)
{
    enum { PGW = 64, PGH = 64, KEY = 0, EDGE = 2 };
    const unsigned char *idx;
    int r, c, opaque = 0, both = 0;

    if (v < 0 || v >= k_r5_flora_tree_count) return 1;   /* unknown -> not upright */
    idx = k_r5_flora_tree_idx[v];
    for (r = 0; r < PGH; r++) {
        const unsigned char *row = idx + r * PGW;
        int lo = -1, hi = -1;
        for (c = 0; c < PGW; c++)
            if (row[c] != KEY) { if (lo < 0) lo = c; hi = c; }
        if (lo < 0) continue;                            /* fully transparent row */
        opaque++;
        if (lo <= EDGE && hi >= PGW - 1 - EDGE) both++;  /* reaches both edges */
    }
    if (opaque <= 0) return 1;
    return (both * 100) >= (opaque * 40);
}

/* [R6 item 7 / R7 item 8] Pick a FLORA page fit to stand upright as ONE tree.
 * Selects on the band PROPERTY (tg_flora_page_is_band) over the whole set rather
 * than a fixed index list, so every band page -- present or future -- is skipped.
 * Returns the FLORA index picked by `pick`, or -1 if no individual page exists. */
static int tg_flora_upright_index(unsigned int pick)
{
    int v, n = 0, avail[TD5_TG_R5_FLORA_N];

    for (v = 0; v < k_r5_flora_tree_count && v < TD5_TG_R5_FLORA_N; v++)
        if (!tg_flora_page_is_band(v)) avail[n++] = v;
    if (n <= 0) return -1;
    return avail[pick % (unsigned)n];
}

/* Inverse of tg_tree_slot: tree variant drawn on `page`, or -1 if it is not a
 * tree page at all. The billboard writer only has the page id to go on. */
static int tg_tree_variant_of_page(int page)
{
    if (page == TD5_TG_PAGE_TREE) return 0;
    if (page >= TD5_TG_PAGE_TREE_EXTRA &&
        page <  TD5_TG_PAGE_TREE_EXTRA + TD5_TG_TREE_VARIANTS - 1)
        return page - TD5_TG_PAGE_TREE_EXTRA + 1;
    return -1;
}

/* Does `page` need mirror-and-duplicate? Only a real (borrowed) tree page that
 * the measurement above found to be one half of a pair. */
int tg_tree_page_is_half(int page)
{
    const int v = tg_tree_variant_of_page(page);
    if (v < 0 || !k_tree_pages[v].half) return 0;
    if (!tg_real_textures_enabled()) return 0;   /* procedural pages are whole */
    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_TREE_MIRROR=0 restores the raw
     * half page, i.e. the sliced-tree look that was reported. */
    return td5_env_flag_on("TD5RE_AUTOTRACK_TREE_MIRROR");
}

/* Push a verge setback (`gap`, measured from the road EDGE) out far enough that
 * nothing standing on it overlaps a branch carriageway. Returns `gap` unchanged
 * on the +ve lateral, which no corridor bows into.
 *
 * This used to carry its own tg_flora_branch_reach, one of the two hand-rolled
 * copies of the fork arithmetic the CARRIAGEWAY QUERY section was written to
 * retire. That copy assumed the corridor was a FIXED half carriageway
 * (width*0.25 out from its centre), which stopped being true once the corridor
 * learned to widen and taper, so it UNDER-REPORTED on a widened branch and
 * trees kept landing on it. Retired onto tg_carriageway_clear_gap at the
 * round-2 merge (2026-08-27).
 *
 * Off a fork the authority's reach floors at the main road's half width, so
 * `need` is just the margin -- 600, well under the 800 minimum tree setback and
 * the 11000 treeline setback, i.e. no change to placement away from a fork. */
double tg_flora_gap_clear(const TG_NodeList *nl, int si, double side,
                                 double gap)
{
    if (side > 0.0) return gap;
    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_FLORA_CLEAR=0 restores the old
     * placement, i.e. trees standing on the branch. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FLORA_CLEAR")) return gap;
    return tg_carriageway_clear_gap(nl, si, side, gap,
                                    TD5_TG_FLORA_BRANCH_MARGIN);
}

TG_R12FloraRec s_r12_flora[TD5_TG_R12_FLORA_MAX];

int s_r12_flora_n;

unsigned char s_r12_band_why[TD5_TG_MAX_SPANS];

int s_r12_band_seen;

static void tg_r12_flora_note(int si, const char *kind, int page, double side,
                              double cx, double cz, double tw, double th)
{
    TG_R12FloraRec *r;
    if (s_r12_flora_n >= TD5_TG_R12_FLORA_MAX) return;
    r = &s_r12_flora[s_r12_flora_n++];
    r->si = si; r->kind = kind; r->page = page; r->side = side;
    r->cx = cx; r->cz = cz; r->tw = tw; r->th = th;
}

int s_r12_flora_rejects;

int tg_r12_flora_accept(int si, const char *kind, int page, double side,
                               double cx, double cz, double tw, double th)
{
    if (td5_env_flag_on("TD5RE_R12_FLORA_SPACE")) {
        int i;
        for (i = 0; i < s_r12_flora_n; i++) {
            const TG_R12FloraRec *r = &s_r12_flora[i];
            double dx, dz, narrow, min_d;
            if (r->si < si - 3 || r->si > si + 3) continue;
            narrow = (r->tw < tw) ? r->tw : tw;
            min_d  = narrow * TD5_TG_R12_SEP_FRAC;
            dx = cx - r->cx; dz = cz - r->cz;
            if (dx * dx + dz * dz < min_d * min_d) {
                s_r12_flora_rejects++;
                return 0;
            }
        }
    }
    tg_r12_flora_note(si, kind, page, side, cx, cz, tw, th);
    return 1;
}

static void tg_r12_band_note(int si, int why)
{
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return;
    s_r12_band_why[si] = (unsigned char)why;
    s_r12_band_seen = 1;
}

/* [R6 FLORA diag, dev-only] A camera-facing tree billboard is a DISC of radius
 * tw/2 about its plant point, so "clears the verge at span si" is not the same
 * as "clears the road": where the road CURVES back under a wide tree, or on a
 * fork, the tree can overhang a carriageway one or more spans away while its own
 * span's gap looks fine. This dumps, for a tree planted at world (cx,cz), the
 * SWEPT nearest approach of its disc to any road surface within +/-6 spans, as a
 * signed clearance (negative = the disc overlaps the road = spill). Read-only;
 * gated by TD5RE_FLORA_DIAG=1 and limited to spans near TD5RE_FLORA_DIAG_SPAN so
 * race.log stays legible. Confirms item 7/18 by number, not by eye. */
void tg_flora_diag(const TG_NodeList *nl, int si, const char *kind,
                          int page, double side, double tw, double th,
                          double cx, double cz)
{
    int tgt, m, worst = -1;
    double min_clear = 1e18;
    if (!td5_env_flag_on("TD5RE_FLORA_DIAG")) return;
    tgt = td5_env_int("TD5RE_FLORA_DIAG_SPAN", 1413, 0, 100000);
    if (si < tgt - 8 || si > tgt + 8) return;
    for (m = si - 6; m <= si + 6; m++) {
        const TG_Node *nm, *nn;
        double dx, dz, lat, lon, seglen, clear;
        if (m < 0 || m + 1 >= nl->count) continue;
        nm  = &nl->v[m];
        nn  = &nl->v[m + 1];
        dx  = cx - nm->x; dz = cz - nm->z;
        lon = dx * nm->tx + dz * nm->tz;          /* along the span's tangent */
        seglen = sqrt((nn->x - nm->x) * (nn->x - nm->x) +
                      (nn->z - nm->z) * (nn->z - nm->z));
        /* only spans the tree is actually BESIDE (not a far stretch the road
         * doubles back near) -- a slab of one span length, small end slop. */
        if (lon < -600.0 || lon > seglen + 600.0) continue;
        lat = dx * nm->tz - dz * nm->tx;          /* signed lateral offset */
        if (lat < 0.0) lat = -lat;
        clear = lat - nm->width * 0.5 - tw * 0.5;  /* disc edge to road edge */
        if (clear < min_clear) { min_clear = clear; worst = m; }
    }
    TD5_LOG_I(LOG_TAG,
        "flora-diag: si=%d %s page=%d side=%+.0f tw=%.0f th=%.0f "
        "reach=%.0f swept_clear=%.0f @m=%d %s",
        si, kind, page, side, tw, th,
        tg_carriageway_reach(nl, si, side), min_clear, worst,
        (min_clear < 0.0) ? "SPILL" : "ok");
}

const TG_PropPage k_prop_pages[TD5_TG_PROP_COUNT] = {
    {  300,  700, 1, 1,    0, TG_PROP_PERSON },  /* 0 L001 p316 spectator   */
    {  300,  700, 1, 1,    0, TG_PROP_PERSON },  /* 1 L001 p318 spectator   */
    { 1400, 1700, 1, 1,    0, TG_PROP_STATUE },  /* 2 L004 p446 lion        */
    { 1800, 4200, 1, 1,    0, TG_PROP_STATUE },  /* 3 L014 p274 monument    */
    { 1300, 1000, 1, 1,    0, TG_PROP_ANIMAL },  /* 4 L001 p341 sheep       */
    { 1300, 1600, 1, 1,    0, TG_PROP_ANIMAL },  /* 5 L003 p453 deer        */
    {  800,  800, 2, 3, 2500, TG_PROP_LAMP   }   /* 6 L001 p378 lamp glow   */
};

int tg_prop_slot(int i) { return TD5_TG_PAGE_PROP + i; }

const TG_RoadSurf k_road_surf[TD5_TG_ROAD_VARIANTS] = {
    { 1, 0, RS_TARMAC },   /* dry asphalt, full grip (base ROAD page) */
    { 4, 1, RS_GRAVEL },   /* packed gravel */
    { 3, 2, RS_DIRT   },   /* dirt */
    { 6, 3, RS_ICE    },   /* ice / snow -- slippery */
    { 5, 4, RS_COBBLE }    /* cobble -- draggy stone */
};

int tg_road_slot(int v)
{
    return v == 0 ? TD5_TG_PAGE_ROAD : (TD5_TG_PAGE_ROAD_EXTRA + v - 1);
}

/*                                                clim urb  brdg tunl rpt */
const TG_Biome k_biomes[] = {
    /* CITY: ~8.4x11.5 wu cells, ~3 floors, on the curb, big sparse towers.
     * Few bridges (a city road crosses at grade), the odd underpass.
     *
     * [R9 item 13] w_tunnel 0 -> 70. The comment above always said "the odd
     * underpass" and the weight always said never, because until this round a
     * "tunnel" could only be a bored mountain portal and a bore under a city is
     * absurd -- so zero was right. Item 13 makes a CITY tunnel an UNDERPASS, and
     * the user named the city specifically ("the tunnels in the city should
     * represent underpasses"), so leaving this at 0 would have shipped the fix
     * to every biome EXCEPT the one it was asked for. Safe to raise: an
     * underpass run returns 0 from tg_span_in_tunnel, so it moves no road, no
     * elevation and no surface -- it only adds scenery. */
    { "CITY",       9, 2150, 2950, 2, 3, 6000, 350, {0}, 0,
      TD5_TG_PAGE_WALL,   3, 0, TD5_TG_PAGE_GROUND,   6, 1, PP_MONUMENT, -1, 0, RS_TARMAC,
      1, 3,  40,  70, 3 },
    /* FIELDS: sparse deciduous on an open horizon; grazing sheep; dirt road.
     * COUNTRYSIDE = BRIDGES: open farmland is where the road crosses rivers and
     * dry valleys, which is the user's "if it's a countryside there should be
     * more bridges". Nothing to tunnel through. */
    { "FIELDS",     2, 0,0,0,0,0,0, {2, 0},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_SHEEP, 0, RS_DIRT,
      1, 1, 190,  10, 2 },
    /* FOREST: dense mixed deciduous crowding the verge; deer. */
    { "FOREST",    11, 0,0,0,0,0,0, {0, 1, 2},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_DEER, 0, RS_TARMAC,
      1, 0, 110,  70, 2 },
    /* INDUSTRIAL: wider squat sheds; gravel yards. The transition biome between
     * a city and open country -- see the adjacency reasoning below. */
    { "INDUSTRIAL", 6, 2560, 2300, 1, 2, 8000, 600, {0}, 0,
      TD5_TG_PAGE_WALL,  63, 0, TD5_TG_PAGE_GROUND,   3, 1, -1, -1, 0, RS_GRAVEL,
      1, 2,  90,  40, 2 },
    /* ALPINE: conifers + snow conifers; deer; icy road.
     * MOUNTAINS = TUNNELS, the user's second example. Also bridges (viaducts
     * across the valleys between the bores), but tunnels dominate. */
    { "ALPINE",     8, 0,0,0,0,0,0, {3, 4},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   0, 0, -1, PP_DEER, 0, RS_ICE,
      2, 0, 140, 230, 2 },
    /* COAST: palms; beach crowds + promenade lamps; the sea alongside.
     * Causeways over inlets, headland bores. repeat_max 1 ON PURPOSE: the water
     * emitter keys its sea level and its seaward side off the 150-span cell, so
     * a COAST spanning two cells could step the sea surface or flip which side
     * it is on. Single-cell coasts keep that emitter exactly as correct as it is
     * today. Lift this only together with tg_biome_run_bounds adoption there. */
    { "COAST",      5, 0,0,0,0,0,0, {5, 6},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   6, 1, -1, -1, 1, RS_TARMAC,
      0, 1, 160,  60, 1 },
    /* ORIENTAL: manicured topiary + weeping willow; guardian lions; cobbles. */
    { "ORIENTAL",   9, 0,0,0,0,0,0, {7, 8, 9},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN,   4, 0, PP_LION, -1, 0, RS_COBBLE,
      1, 2, 120,  50, 2 },
    /* ---------------------------------------------------------------------
     * [R8 BIOME item 19] ALPTOWN -- the SNOW-ONLY biome, index 7.
     *
     * It exists to answer the "snowy themed biomes AND CITIES" half of item 19.
     * Before it, `cold` (climate 2) had exactly ONE member, ALPINE, and ALPINE
     * is urbanity 0 (wilderness). Rule 2 caps an urbanity step at 1, so a cold
     * run could never be followed by another cold run: every snow stretch was
     * forced back through a temperate biome. That is the mechanism behind the
     * user's "lurch", and no amount of re-weighting the picker could fix it --
     * the adjacency graph simply had no cold-to-cold edge.
     *
     * ALPTOWN is climate 2 / urbanity 1, so ALPINE(0) <-> ALPTOWN(1) is one
     * urbanity step and one climate step of zero: the first legal cold-to-cold
     * edge. Snow can now chain for as long as the picker wants.
     *
     * ROAD SURFACE IS TARMAC, NOT ICE, ON PURPOSE. A ploughed town road is the
     * real-world answer, and it is also the safe one: with snow chaining, an
     * icy ALPTOWN would put most of a snow seed's length on RS_ICE and put the
     * race-finishes-at-all acceptance at risk. ALPINE keeps repeat_max 2, so
     * the longest continuous ice run stays 300 spans -- the same order as
     * before this change.
     *
     * It reuses the existing WALL / GROUND pages: what a snowy town LOOKS like
     * is TERRAIN's and VARIETY's call this round, not BIOME's.
     *
     * NOT counted in TD5_TG_BIOME_COUNT. Every `% TD5_TG_BIOME_COUNT` in this
     * file stays modulo 7, so a non-snow seed's layout is bit-identical to the
     * pre-R8 build. Only the snow-coherent path widens the draw to 8. */
    { "ALPTOWN",    7, 2300, 2600, 1, 2, 6500, 400, {0}, 0,
      TD5_TG_PAGE_WALL,  63, 0, TD5_TG_PAGE_GROUND,   4, 1, -1, PP_DEER, 0, RS_TARMAC,
      2, 1, 120, 160, 3 }
};

/* ==========================================================================
 * BIOME ADJACENCY  (feedback R2 item 23)
 *
 * THE COMPLAINT was that biome transitions are abrupt and that the biome does
 * not change what gets built. Both come from the same line of code: the biome
 * used to be a raw hash of (span / 150), so consecutive runs were INDEPENDENT
 * draws from all 7 biomes with a hard cut at the boundary. That produces snow
 * conifers ending and palm trees starting on the same span, which is the
 * "incompatibility due to difference" the user asked to be reasoned about.
 *
 * HOW LONG IS A RUN? A span is TD5_TG_SPAN_LENGTH (1500) world units and a lane
 * is TD5_TG_LANE_WIDTH (1500) world units. Taking a lane as a real 3.5 m lane,
 * one span is ~3.5 m and a 150-span run is ~525 m of road. That number is what
 * makes the rules below obvious rather than arbitrary: half a kilometre.
 *
 * RULE 1 -- CLIMATE, |climate[a] - climate[b]| <= 1.
 *   You do not drive out of snowbound conifers into a palm-lined beach road in
 *   half a kilometre. Nothing in the world does that; a mountain road reaches
 *   the sea through a treeline, then foothills, then the coast. Adjacent runs
 *   must be at most one step apart on warm(0) / temperate(1) / cold(2), so
 *   ALPINE-COAST is rejected outright and can only occur with a temperate run
 *   (FIELDS, FOREST) between them.
 *   This rule also does the grip work for free: RS_ICE lives only in ALPINE and
 *   ALPINE can only touch temperate biomes, so the car never steps from cobbles
 *   straight onto ice at speed.
 *
 * RULE 2 -- URBANITY, |urbanity[a] - urbanity[b]| <= 1.
 *   Real roads step into a city, they do not cut into it: wilderness, farmland,
 *   the edge with its sheds and yards, then the dense centre. A CITY run whose
 *   neighbour is FOREST reads as two tracks spliced together. Requiring one step
 *   on wild(0) / rural(1) / edge(2) / urban(3) forces INDUSTRIAL or ORIENTAL to
 *   stand between CITY and open country, which is exactly the suburban belt the
 *   user is asking for -- it is not a separate feature, it falls out of the rule.
 *
 * RULE 3 -- REPETITION, up to repeat_max consecutive cells.
 *   The rules above are constraints; this is the one that lengthens things.
 *   A biome may hold several cells in a row, and the urban biomes are allowed
 *   the most (CITY 3 cells = ~1.5 km of city), which is the user's "urban and
 *   suburban areas longer". COAST is pinned to 1 for the emitter reason in its
 *   table row.
 *
 * The graph these rules leave is connected with no dead ends (every biome has at
 * least two legal successors), so the picker below can never get stuck and never
 * needs a "give up and take anything" escape that would reintroduce the bad cuts.
 * ========================================================================== */
static int tg_biome_compatible(int a, int b)
{
    int dc = k_biomes[a].climate  - k_biomes[b].climate;
    int du = k_biomes[a].urbanity - k_biomes[b].urbanity;
    if (dc < 0) dc = -dc;
    if (du < 0) du = -du;
    return dc <= 1 && du <= 1;
}

static unsigned char s_biome_cell[TD5_TG_BIOME_CELLS];

static int           s_biome_laid_out = 0;

/* Deterministic per-cell hash. Not tg_rand(): the biome layout must not consume
 * the geometry RNG stream, or adding a biome rule would move the road. */
static unsigned int tg_biome_hash(unsigned int seed, int cell, unsigned int salt)
{
    unsigned int h = seed ^ (salt * 0x9E3779B9u);
    h += (unsigned)cell * 2654435761u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

/* ==========================================================================
 * SNOW COHERENCE  ([R8] BIOME, seed 777 item 19, first clause)
 *
 * THE COMPLAINT, verbatim: "if you include snow in the seed there has to be a
 * prevalence for snowy themed biomes and cities TO AVOID MAJOR CHANGES".
 * The stated reason is the important half. Seed 777 lays out
 *   INDUSTRIAL COAST FOREST *ALPINE* FIELDS INDUSTRIAL ORIENTAL FIELDS
 * -- one 150-span run of snow with five non-snowy runs around it, including a
 * palm-lined COAST 300 spans earlier. The adjacency rules (R2 item 23) already
 * forbid ALPINE touching COAST, so nothing is "wrong" span-by-span; the defect
 * is at the level of the WHOLE SEED. A track is not a sequence of locally legal
 * transitions, it is one place, and one place does not contain both a snowbound
 * pass and a tropical beach.
 *
 * SO THE RULE IS SEED-LEVEL, NOT CELL-LEVEL:
 *   1. Lay the grid out exactly as before. Ask one question of the result:
 *      does ALPINE appear anywhere the track actually reaches?
 *   2. If it does, THROW THAT LAYOUT AWAY and lay the seed out again in snow
 *      mode: warm biomes (climate 0, i.e. COAST) are struck from the draw
 *      entirely, ALPTOWN joins it, and a cold predecessor prefers a cold
 *      successor 80% of the time (a temperate one, 60%).
 *
 * Two-pass rather than one because the trigger has to be a property of the
 * seed, not of the cell being filled: you cannot know at cell 3 whether cell 9
 * will roll snow. Pass 1 is pure and cheap (22 cells), so the cost is nil.
 *
 * WHY 80/60 AND NOT 100. At 100% every snow seed becomes 10/10 cold and stops
 * varying at all, which trades one monotony for another. Measured across 18
 * seeds (7 of them snow seeds), 80/60 leaves 60 of 70 cells cold, 0 warm, and
 * cuts climate flips from 31 to 8 -- snow dominates without the track becoming
 * a single texture. See the changelog entry for the full before/after table.
 *
 * WHY NON-SNOW SEEDS ARE UNTOUCHED, and why that matters this round: pass 1 is
 * the unmodified algorithm over the unmodified modulo-7 draw, so a seed with no
 * ALPINE never enters pass 2 and its layout is bit-identical to the pre-R8
 * build. Seed 99991 -- the reference layout the other seven R8 areas are
 * testing their span numbers against -- has no ALPINE and is therefore
 * unchanged BY CONSTRUCTION, not by luck.
 *
 * KNOB: TD5RE_R8_BIOME_SNOW, default ON. td5_env_flag_on returns 1 when the
 * variable is UNSET, which is the default-on behaviour wanted here; =0 restores
 * the pre-R8 selection exactly.
 * ========================================================================== */

/* May biome b be drawn at all in this mode? Snow mode strikes warm (climate 0)
 * biomes; normal mode strikes ALPTOWN by keeping the draw at modulo 7. */
static int tg_biome_snow_allows(int b) { return k_biomes[b].climate >= 1; }

static int tg_biome_repeat_max(int b)
{
    if (k_biomes[b].water && td5_env_int("TD5RE_R8_BIOME_SEA", 0, 0, 1))
        return TD5_TG_COAST_REPEAT_SEA;
    return k_biomes[b].repeat_max;
}

/* One layout pass. `snow` widens the draw to include ALPTOWN, bans warm biomes
 * and biases succession toward cold. Writes s_biome_cell. */
static void tg_biome_layout_pass(unsigned int seed, int snow)
{
    const int nb = snow ? TD5_TG_BIOME_COUNT_SNOW : TD5_TG_BIOME_COUNT;
    int cell, run_len = 1;

    if (!snow) {
        s_biome_cell[0] = (unsigned char)(tg_biome_hash(seed, 0, 1u)
                                          % TD5_TG_BIOME_COUNT);
    } else {
        /* Same hash, then walk forward to the first snow-legal biome, so the
         * starting cell is drawn from the same stream and only the FILTER
         * differs between the two passes. */
        unsigned int h0 = tg_biome_hash(seed, 0, 1u);
        int t, first = 0;
        for (t = 0; t < nb; t++) {
            int c0 = (int)((h0 + (unsigned)t) % (unsigned)nb);
            if (tg_biome_snow_allows(c0)) { first = c0; break; }
        }
        s_biome_cell[0] = (unsigned char)first;
    }

    for (cell = 1; cell < TD5_TG_BIOME_CELLS; cell++) {
        int prev = s_biome_cell[cell - 1];
        unsigned int h = tg_biome_hash(seed, cell, 2u);
        int cand, chosen = -1, tries;

        /* Extend the current biome first, up to its repeat_max. The 0..99 draw
         * against a flat 45% keeps runs varied -- always extending to the cap
         * would make every city exactly repeat_max cells long. */
        if (run_len < tg_biome_repeat_max(prev) && (h % 100u) < 45u) {
            s_biome_cell[cell] = (unsigned char)prev;
            run_len++;
            continue;
        }
        /* [R8] Snow mode's COLD-FIRST pass. Restricted to climate 2 candidates;
         * if none is compatible it simply falls through to the normal scan
         * below, so this can only ever bias the choice, never fail it. Keyed on
         * a different slice of the same hash word (>>16) than the extend draw
         * (%100) and the candidate rotation (>>8), so the three decisions do not
         * correlate. */
        if (snow && ((h >> 16) % 100u) < (k_biomes[prev].climate == 2 ? 80u : 60u)) {
            for (tries = 0; tries < nb; tries++) {
                cand = (int)(((h >> 8) + (unsigned)tries) % (unsigned)nb);
                if (cand == prev) continue;
                if (k_biomes[cand].climate != 2) continue;
                if (!tg_biome_compatible(prev, cand)) continue;
                chosen = cand;
                break;
            }
        }
        /* Otherwise walk the biome list from a hashed offset and take the first
         * COMPATIBLE one that is not the biome we just left (we already decided
         * not to extend it). Scanning from a rotating offset rather than
         * rejection-sampling keeps this bounded and deterministic. */
        if (chosen < 0) {
            for (tries = 0; tries < nb; tries++) {
                cand = (int)(((h >> 8) + (unsigned)tries) % (unsigned)nb);
                if (cand == prev) continue;
                if (snow && !tg_biome_snow_allows(cand)) continue;
                if (!tg_biome_compatible(prev, cand)) continue;
                chosen = cand;
                break;
            }
        }
        /* Unreachable by construction (the adjacency graph has no dead ends),
         * but a silent out-of-range index here would be a crash rather than a
         * cosmetic bug, so it is pinned rather than asserted. */
        if (chosen < 0) chosen = prev;
        s_biome_cell[cell] = (unsigned char)chosen;
        run_len = (chosen == prev) ? run_len + 1 : 1;
    }
    s_biome_laid_out = 1;
}

/* Set by tg_biome_layout so the build log and the element inventory can state
 * plainly whether the snow-coherent path was taken. */
int s_biome_snow_seed = 0;

/* Lay the whole cell grid out once per build, before any emitter asks.
 * nspans_hint is the track's target length: the snow TRIGGER is scanned only
 * over the cells the track will actually reach, because s_biome_cell covers
 * TD5_TG_MAX_SPANS (22 cells / 3300 spans) and a ~1500-span track never sees
 * the tail. Triggering on an ALPINE nobody can drive to would be a silent
 * false positive -- exactly the kind of "the mechanism fired for the wrong
 * reason" the round's method rules exist to catch. */
void tg_biome_layout(unsigned int seed, int nspans_hint)
{
    int used, cell;

    s_biome_snow_seed = 0;
    tg_biome_layout_pass(seed, 0);

    if (!td5_env_flag_on("TD5RE_R8_BIOME_SNOW")) return;

    used = (nspans_hint > 0 ? nspans_hint : TD5_TG_MAX_SPANS)
           / TD5_TG_BIOME_RUN + 1;
    if (used > TD5_TG_BIOME_CELLS) used = TD5_TG_BIOME_CELLS;

    for (cell = 0; cell < used; cell++) {
        if (s_biome_cell[cell] == (unsigned char)TD5_TG_BIOME_ALPINE) {
            s_biome_snow_seed = 1;
            break;
        }
    }
    if (s_biome_snow_seed) tg_biome_layout_pass(seed, 1);
}

/* HARD cell assignment -- no blending. This is the biome that OWNS the span, and
 * it is what per-run state (road surface, sea level, run bounds) must key off:
 * a dithered road-surface class would flip grip from span to span. */
int tg_biome_cell_index(int si)
{
    int cell;
    if (si < 0) si = 0;
    cell = si / TD5_TG_BIOME_RUN;
    if (cell >= TD5_TG_BIOME_CELLS) cell = TD5_TG_BIOME_CELLS - 1;
    if (!s_biome_laid_out)   /* asked before a build laid the grid out */
        return (int)(((unsigned)cell * 2654435761u) >> 27) % TD5_TG_BIOME_COUNT;
    return (int)s_biome_cell[cell];
}

/* Merged extent of the biome run containing si (consecutive cells of the same
 * biome collapse into one run). Offered for the emitters that currently do
 * `si / TD5_TG_BIOME_RUN` arithmetic to derive per-run state -- with repeated
 * cells that arithmetic sees a 300-span city as two runs. Not yet adopted by
 * them; see the COAST repeat_max note. */
void tg_biome_run_bounds(int si, int *out_a, int *out_b)
{
    int cell = (si < 0) ? 0 : si / TD5_TG_BIOME_RUN;
    int b = tg_biome_cell_index(si), a = cell, z = cell;
    if (cell >= TD5_TG_BIOME_CELLS) cell = a = z = TD5_TG_BIOME_CELLS - 1;
    while (a > 0 && tg_biome_cell_index((a - 1) * TD5_TG_BIOME_RUN) == b) a--;
    while (z + 1 < TD5_TG_BIOME_CELLS &&
           tg_biome_cell_index((z + 1) * TD5_TG_BIOME_RUN) == b) z++;
    if (out_a) *out_a = a * TD5_TG_BIOME_RUN;
    if (out_b) *out_b = (z + 1) * TD5_TG_BIOME_RUN - 1;
}

/* BLENDED biome for span si -- what the SCENERY emitters should ask.
 *
 * Inside the band around a cell boundary the span is assigned to the incoming or
 * the outgoing biome by a per-span hash whose threshold ramps linearly across
 * the band. That is ordered dithering of a categorical field, and it is the only
 * thing that actually smooths a transition between two DISCRETE biomes: you
 * cannot interpolate "forest" into "city", but you can interleave them, so the
 * trees thin out over ~140 m while the first buildings start appearing, instead
 * of every tree stopping and every building starting on one span.
 *
 * The hash is per-span and seed-independent-of-order, so the interleave is
 * stable across the thousands of calls an emitter makes for the same span. */
int tg_biome_for_span(int si)
{
    int here, cell, off, other, dist;
    unsigned int h, thresh;

    if (si < 0) si = 0;
    here = tg_biome_cell_index(si);
    if (td5_env_int("TD5RE_AUTOTRACK_BIOME_BLEND", TD5_TG_BIOME_BLEND,
                    0, TD5_TG_BIOME_RUN / 2) <= 0)
        return here;

    cell = si / TD5_TG_BIOME_RUN;
    off  = si - cell * TD5_TG_BIOME_RUN;

    if (off < TD5_TG_BIOME_BLEND) {              /* leading edge of this cell */
        other = tg_biome_cell_index((cell - 1) * TD5_TG_BIOME_RUN);
        dist  = off;                              /* 0 = right on the boundary */
    } else if (off >= TD5_TG_BIOME_RUN - TD5_TG_BIOME_BLEND) { /* trailing edge */
        other = tg_biome_cell_index((cell + 1) * TD5_TG_BIOME_RUN);
        dist  = TD5_TG_BIOME_RUN - 1 - off;
    } else {
        return here;                              /* solid core of the run */
    }
    if (other == here) return here;

    /* dist 0 -> even odds; dist == BLEND -> always `here`. */
    thresh = (unsigned)(50 + (50 * dist) / TD5_TG_BIOME_BLEND);
    h = tg_biome_hash((unsigned)si, si, 3u);
    return ((h % 100u) < thresh) ? here : other;
}

/* Per-biome feature weighting, as a PERCENT of the emitter's own global rate:
 * 100 = unchanged, 0 = never in this biome, 200 = twice as likely.
 *
 * Offered as accessors rather than as raw table reads so the bridge and tunnel
 * emitters (owned elsewhere) need one line each and stay unaware of the biome
 * struct. Keyed on the HARD cell index, not the blended one: a bridge is a
 * hundred spans of structure and must not be decided by a dithered edge span.
 *
 * Suggested use at the emitter's existing gate:
 *     if (roll % 100 >= (base_pct * tg_biome_bridge_pct(si)) / 100) return 0; */
int tg_biome_bridge_pct(int si)
{
    return k_biomes[tg_biome_cell_index(si)].w_bridge;
}

int tg_biome_tunnel_pct(int si)
{
    return k_biomes[tg_biome_cell_index(si)].w_tunnel;
}

/* [R3 BLOCK] item 4: HARD cell index (not blended) -- a block turn is a shape
 * decision, so it must key off the same solid cell the road surface does. */
int tg_biome_span_is_city(int si)
{
    return !strcmp(k_biomes[tg_biome_cell_index(si)].name, "CITY");
}

/* [R7 CITY item 7] Continuity of the city's STRUCTURAL edge -- raised sidewalk +
 * kerb HEIGHT + roadside railing, and the street-wall/tree choice that carries
 * them -- to the TRUE end of a city run. The scenery emitters normally read the
 * BLENDED biome (tg_biome_for_span), whose ~20-span dither band around a cell
 * boundary lets a trailing city span resolve to the neighbour (and a leading
 * neighbour span to the city). At a city edge that dithers the kerb height, the
 * railing and the wall/tree choice per span, and bleeds the pavement raggedly
 * into the next biome -- the railing present on one side only, the "transition
 * on span 455 doesn't keep continuity" report (seed 99991: CITY ends at 449, the
 * pavement + a one-sided railing bleed on to ~479). A raised kerb and a guard
 * rail are per-RUN structure, not a dithered categorical field, so at a CITY
 * boundary they key off the HARD cell the road surface already uses
 * (tg_biome_cell_index): solid to the last city span, then a clean edge.
 *
 * SCOPED TO CITY EDGES ONLY. Where the hard and blended biome already agree, or
 * where NEITHER is CITY, this returns the blended index unchanged -- every
 * non-city transition keeps its dither (the trees-thinning-into-buildings blend
 * this replaces only at the city line). Default ON; TD5RE_R7_CITY_CONT=0 restores
 * the fully-blended edge for an A/B. */
int tg_biome_index_is_city(int idx)
{
    return !strcmp(k_biomes[idx].name, "CITY");
}

/* [R11 GUARD item 5] "Does this biome carry a RAISED PAVEMENT?" -- exactly
 * tg_city_sidewalk_w's `> 0` test, spelled out here because that helper is
 * defined further down the file. A biome is paved iff it builds facades on a
 * cell grid rather than billboard trees; the paved set is CITY, INDUSTRIAL and
 * ALPTOWN, the rest get the flat verge band instead. */
static int tg_biome_index_is_paved(int idx)
{
    return !k_biomes[idx].billboard && k_biomes[idx].cell_w > 0;
}

/* How built-up is span si, 0.0 (bare outskirt) .. 1.0 (full town character)?
 *
 * 1.0 everywhere except the leading TD5_TG_TOWN_RAMP spans of a PAVED biome run
 * whose predecessor run is UNPAVED -- i.e. exactly the wilderness-to-town edges,
 * and not the town-to-town or country-to-country ones. Keyed on the MERGED run
 * (tg_biome_run_bounds) so a 300-span two-cell ALPTOWN ramps once, at its true
 * start, rather than again at its internal cell line; and on the HARD cell, so
 * it agrees span for span with the structural edge R11 GUARD hardened.
 *
 * QUANTISED to the facade period. A per-span ramp would change a run's height
 * halfway along it and saw-tooth its roofline, which is the identical mistake
 * tg_facade_floors keys on the RUN hash to avoid. Snapping the distance to whole
 * superblocks makes the ramp constant across any one building.
 *
 * Default ON; TD5RE_R11_TOWN_RAMP=0 restores the hard switch-on for an A/B. */
double tg_town_ramp(int si)
{
    int a, z, d;

    if (!td5_env_flag_on("TD5RE_R11_TOWN_RAMP")) return 1.0;
    if (si < 0) return 1.0;
    if (!tg_biome_index_is_paved(tg_biome_cell_index(si))) return 1.0;
    tg_biome_run_bounds(si, &a, &z);
    /* A town that starts at span 0 has no wilderness before it to ease out of,
     * and TD5RE_AUTOTRACK_START_CITY deliberately forces a solid frontage
     * there. */
    if (a <= 0) return 1.0;
    if (tg_biome_index_is_paved(tg_biome_cell_index(a - 1))) return 1.0;
    d = si - a;
    if (d >= TD5_TG_TOWN_RAMP) return 1.0;
    if (d < 0) d = 0;
    /* Quantise to the block, then take the block's MIDPOINT, not its leading
     * edge. Measured: with the leading edge the first block sits at exactly
     * r = 0.0, which opens EVERY frontage and empties the town's first 22 spans
     * outright -- that is not a ramp, it is the same hard edge moved 22 spans
     * along. The midpoint gives 0.17 / 0.50 / 0.83 across the three blocks, so
     * the first block keeps a few low buildings and the density climbs from
     * there. */
    d = (d / TD5_TG_FACADE_PERIOD) * TD5_TG_FACADE_PERIOD
      + TD5_TG_FACADE_PERIOD / 2;
    if (d > TD5_TG_TOWN_RAMP) d = TD5_TG_TOWN_RAMP;
    return (double)d / (double)TD5_TG_TOWN_RAMP;
}

/* DENSITY axis: does the ramp open the frontage of the run at (si, left)?
 *
 * Keyed on the RUN id, not the span, so a whole building drops out and the ones
 * that remain are still full-length runs with proper corner returns -- a
 * per-span roll would punch a one-span hole in the middle of a block, which is
 * the R6 item 16 defect. At the boundary span the ramp is 0.0 and every run is
 * open; by the end of the band it is 1.0 and none is. */
int tg_town_ramp_open(int si, int left)
{
    const double r = tg_town_ramp(si);
    if (r >= 1.0) return 0;
    return (int)((tg_facade_run_id(si, left) >> 19) % 1000u)
           >= (int)(r * 1000.0);
}

/* [R11 GUARD item 5] "Remove the slow transition for guardrails and sidewalks
 * -- they should start and stop cleanly."
 *
 * R7 (above) got the PRINCIPLE right and the SCOPE wrong. It hardened the
 * structural edge only where CITY was one of the two biomes, so every OTHER
 * paved/unpaved boundary kept the full 20-span dither. MEASURED on seed
 * 20260901 (TD5RE_R11_GUARD_REPORT=1): 78 dithered spans, of which
 * city-boundary dithers = 0 -- i.e. R7's rule fired on NOTHING in this track
 * and every ramp the user is complaining about ran unchecked.
 *
 * What the dither does to a structural edge, spans 745-755 of that run
 * (ALPTOWN is paved, FIELDS is not):
 *
 *     745 ALPTOWN walk=1200 fence=1 1  rail-owner=kerb-fence
 *     746 FIELDS   walk=0    fence=0 0  rail-owner=roadside
 *     747 ALPTOWN walk=1200 fence=1 1  rail-owner=kerb-fence
 *     748 FIELDS   walk=0    fence=0 0  rail-owner=roadside
 *
 * So a 1200-wide slab with a 130-high kerb and a pedestrian railing appears,
 * vanishes, and reappears every other span for twenty spans, and the roadside
 * armco takes over the edge on exactly the spans the railing skips. That is the
 * "ramps in and out gradually" report, and it is ONE mechanism behind both
 * halves of the item: the guardrail and the sidewalk are the same edge.
 *
 * A kerb, a pavement and the barrier that owns their edge are per-RUN
 * STRUCTURE, not a dithered categorical field -- you cannot half-build a
 * pavement the way you can thin a forest out into a field. So whenever the hard
 * cell and the dithered span disagree about whether there IS a pavement, the
 * hard cell wins and the whole structural edge changes on one span.
 *
 * Deliberately NOT a blanket "always return hard": a FOREST/FIELDS boundary is
 * two unpaved biomes and its tree-thinning dither is the good kind of blend --
 * that one still gets the full 20 spans. Only a boundary that moves STRUCTURE
 * snaps. Default ON; TD5RE_R11_GUARD_HARDEDGE=0 restores R7's city-only scope
 * for an A/B. */
int tg_scenery_biome_index(int si)
{
    int hard = tg_biome_cell_index(si);
    int soft = tg_biome_for_span(si);
    if (hard == soft) return soft;
    if (td5_env_flag_on("TD5RE_R11_GUARD_HARDEDGE") &&
        tg_biome_index_is_paved(hard) != tg_biome_index_is_paved(soft))
        return hard;
    if (td5_env_flag_on("TD5RE_R7_CITY_CONT") &&
        (tg_biome_index_is_city(hard) || tg_biome_index_is_city(soft)))
        return hard;
    return soft;
}

/* [R5 CROSS item 12] Is span si's road surface plain tarmac? The pedestrian
 * ZEBRA page (white bars on a dark base) is authored for asphalt; on the pale
 * gravel of INDUSTRIAL or the cobble of ORIENTAL the white bars sit on an
 * already-light surface and read as a mismatched patch ("the road crossing
 * texture looks out of place ... near this road texture", span 934 = INDUSTRIAL
 * gravel). The side-street MOUTH (crossstreet asphalt) uses the biome's own road
 * page and is unaffected; only the zebra decal keys off this. */
int tg_span_surface_is_tarmac(int si)
{
    return k_biomes[tg_biome_cell_index(si)].road_surf == RS_TARMAC;
}

/* True when span si's surface must ignore its biome and be plain tarmac.
 *
 * Weather does not fall inside a bore. In ALPINE the biome surface is RS_ICE,
 * so a tunnelled ALPINE stretch got the icy grip class AND the snow-and-ice
 * road page under a roof that no snow could ever reach -- the road inside read
 * as an ice rink. The same argument covers the other weather-ish surfaces
 * (nothing washes gravel or grows moss on a sheltered carriageway), so the rule
 * is simply: inside a tunnel, dry asphalt.
 *
 * Single definition, used by both accessors below so the GRIP the car feels and
 * the TEXTURE it drives over can never disagree. Gated so the old behaviour is
 * one env var away; default ON, it is a fix. */
static int tg_span_tunnel_tarmac(int si)
{
    return tg_span_in_tunnel(si) &&
           td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_TARMAC");
}

/* Strip surface byte for span si: low nibble = the biome's drivable GRIP class
 * (what td5_track.c surface_type_for_span_lane reads for the centre lanes when
 * lane_bitmask is 0), high nibble left at 1 (the verge class, unused at mask 0). */
int tg_surface_attr(int si)
{
    const TG_Biome *b;
    if (tg_span_tunnel_tarmac(si))
        return 0x10 | (k_road_surf[RS_TARMAC].grip_class & 0x0F);
    /* [R2 item 23] HARD cell index, never the blended one: the blend dithers
     * span by span, and a grip class that alternates ice/tarmac down a straight
     * is a spin, not a transition. Scenery blends; the road does not. */
    b = &k_biomes[tg_biome_cell_index(si)];
    return 0x10 | (k_road_surf[b->road_surf].grip_class & 0x0F);
}

/* Road texture page for span si, from the biome's surface. */
int tg_road_page(int si)
{
    const TG_Biome *b;
    /* [R3 item 11] A bridge DECK is not tarmac. The biome road pages carry lane
     * paint and asphalt grain that tile down the span, and on a raised deck that
     * repeat reads as a stack of identical road tiles rather than a structure.
     * Swap in the dedicated deck page (cast concrete with a transverse seam per
     * span) so the crossing looks built, not paved. Bridge runs are near-straight
     * (item 15), so the transverse seam does not shear. NOT inside a tunnel: a
     * 40-span bridge run can overlap a 20-span tunnel run (seed 99991 has this at
     * spans 1340-1359), and there the bridge geometry is suppressed for the
     * enclosed section, so a deck seam would belong to nothing -- keep the
     * tunnel's dry tarmac. */
    if (tg_span_in_bridge_run(si) && !tg_span_in_tunnel(si))
        return TD5_TG_PAGE_BRIDGE_DECK;
    if (tg_span_tunnel_tarmac(si))
        return tg_road_slot(k_road_surf[RS_TARMAC].page_var);
    /* Same hard index as tg_surface_attr -- the TEXTURE the car drives over and
     * the GRIP it feels must never disagree, which is only guaranteed while both
     * read the same function. */
    b = &k_biomes[tg_biome_cell_index(si)];
    return tg_road_slot(k_road_surf[b->road_surf].page_var);
}

int tg_topo_enabled(void)
{
    return td5_env_flag_on("TD5RE_R9_TOPO");
}

/* TG_TopoChain, tg_topo_chain and tg_topo_drop_at are declared UP with
 * TG_GroundProf (above the coastline emitter) -- see the note there. */

/* [C3] How far this span-side's ground may reach before another part of the
 * track intervenes, as a distance from THIS span's road edge. Returns a huge
 * number where nothing intervenes.
 *
 * This is the query item 7 is about. The user's words: "you gotta take into
 * consideration if the nearby geometry is touching another road and a
 * downwards slope". At a U-turn the two legs are a few thousand units apart,
 * and each lays a 12000-unit skirt at ITS OWN height straight across the
 * other's -- two surfaces in one place, disagreeing. R8's GUARD area MEASURED
 * exactly this on this seed ("spans 643..647 lies over the carriageway at
 * spans 626..629 at dy +250..+450, the road doubles back on itself at a
 * different height") and answered it by letting the skirt cross from BELOW,
 * which stops it drawing over the tarmac but leaves the two surfaces
 * interpenetrating in the open ground either side. Ending the chain at the
 * other road instead makes the two carriageways SHARE their ground: this
 * side's surface runs up to that road's edge, and that road's own skirt
 * carries on from there. Connected, by construction, with one authority.
 *
 * Main ring only (s_ring_len): a fork's branch corridor is appended past the
 * ring and is already cleared by tg_carriageway_reach / tg_ground_branch_clear,
 * which know its bow. Span separation is measured AROUND the ring so the
 * start/finish join does not read as a foreign road. */
static double tg_topo_road_cap(const TG_NodeList *nl, int si, int is_left)
{
    const int ring = (s_ring_len > 1 && s_ring_len <= nl->count)
                   ? s_ring_len : nl->count;
    const TG_Node *n;
    double ex, ez, ux, uz, half, best = 1e30;
    int j;

    /* DELIBERATELY NOT gated on tg_topo_enabled(): this is the MEASUREMENT of
     * C3 as well as its input, and a query that returns "nothing intervenes"
     * whenever the fix is off makes the before/after sweep report zero
     * violations before the fix -- which is how a round measures its own knob
     * instead of the world. The gate belongs at the two places that APPLY the
     * cap (tg_ground_side and tg_emit_far_band), and it is there. */
    if (!nl || si < 0 || si >= ring) return best;
    n = &nl->v[si];
    half = tg_road_half_width(nl, si);
    /* Outward lateral unit, same convention as tg_flora_plant. */
    ux = n->tz * (is_left ? 1.0 : -1.0);
    uz = -n->tx * (is_left ? 1.0 : -1.0);
    ex = n->x + ux * half;
    ez = n->z + uz * half;

    for (j = 0; j < ring; j++) {
        double dx, dz, along, perp, w, lim;
        int sep = j - si;
        if (sep < 0) sep = -sep;
        if (ring - sep < sep) sep = ring - sep;      /* around the ring */
        if (sep <= TD5_TG_TOPO_SELF_SPANS) continue;
        dx = nl->v[j].x - ex; dz = nl->v[j].z - ez;
        along = dx * ux + dz * uz;
        if (along <= 0.0) continue;                  /* behind this side */
        perp  = dx * uz - dz * ux;
        if (perp < 0.0) perp = -perp;
        w = tg_road_half_width(nl, j) + TD5_TG_TOPO_ROAD_MARGIN;
        /* The ray only meets that road if it passes within its own width. Half
         * a span length of slack: consecutive nodes are TD5_TG_SPAN_LENGTH
         * apart, so a ray crossing BETWEEN two of them must still see them. */
        if (perp > w + (double)TD5_TG_SPAN_LENGTH * 0.5) continue;
        lim = along - w;
        if (lim < TD5_TG_TOPO_MIN_VERGE) lim = TD5_TG_TOPO_MIN_VERGE;
        if (lim < best) best = lim;
    }
    return best;
}

/* [C2] Where a falling side must reach before it is allowed to end unwalled.
 * `so` is the skirt's own outer distance, `drop` the total fall from the road
 * edge to the outer ring. A flat side needs nothing; a side that has fallen
 * `drop` must run out at TD5_TG_TOPO_RUNOUT, hence drop/RUNOUT of horizontal
 * beyond the skirt. This is the arithmetic behind "make sure it goes further
 * than usual", and it is proportional to the SLOPE, not a flat bonus. */
static double tg_topo_runout_reach(double so, double drop, double base_reach)
{
    double need;
    if (!tg_topo_enabled()) return base_reach;
    if (drop <= TD5_TG_TOPO_OPEN_DROP) return base_reach;
    need = so + drop / TD5_TG_TOPO_RUNOUT;
    if (need > TD5_TG_TOPO_MAX_REACH) need = TD5_TG_TOPO_MAX_REACH;
    return (need > base_reach) ? need : base_reach;
}

/* [R6 item 6] How far the flat verge (near ground skirt) reaches outward. At the
 * historical 24000 the skirt is a wide, near-flat apron that on a DESCENT
 * projects over the road ahead and hides it -- seed 99991 span 188 (a straight,
 * gently descending 4-lane avenue) vanished behind its own verge. PROVEN by an
 * A/B frame: the road, the car and everything forward reappeared the moment the
 * skirt was pulled in, with far-band and buildings still on. 12000 still reaches
 * well past the sidewalks, and the far background band starts exactly where the
 * skirt ends (tg_ground_side is the far-band's source too), so the backdrop
 * simply comes in with it -- no gap, no bare ground. Uniform, so the per-group
 * far-band samples stay consistent with the per-span skirt. Default ON;
 * TD5RE_AUTOTRACK_VERGE_NARROW=0 restores the old 24000 apron for an A/B. */
double tg_verge_reach(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_VERGE_NARROW")
         ? TD5_TG_VERGE_REACH : TD5_TG_GROUND_WIDTH;
}

/* [R14 OVERPASS item 3] How far the FLAT skirt must reach at span si so the
 * ground stays under the deck instead of sinking away from it. 0 = this span is
 * not near a crossing and the ordinary verge stands.
 *
 * tg_far_reach, not the arm's own capped length: the arm is capped per SIDE
 * against other legs of the ring, and a floor that inherited that cap would
 * differ between the two kerbs of one slab for a reason that has nothing to do
 * with this span's ground. The cap belongs to tg_ground_side (the R9 topo road
 * cap says the same thing, per side, and is applied to whatever this returns),
 * so asking for the uncapped reach here and letting the existing clamp cut it is
 * both cheaper and one authority rather than two. */
static double tg_r14_up_ground_reach(int si)
{
    const double v = tg_verge_reach();
    const double f = tg_far_reach();
    int d;
    if (!td5_env_flag_on("TD5RE_R14_UP_FLOOR")) return 0.0;
    if (!(f > v)) return 0.0;
    d = tg_r14_up_dist(si, TD5_TG_R14_UP_TAPER);
    if (d < 0) return 0.0;
    return v + (f - v) * (1.0 - (double)d / (double)(TD5_TG_R14_UP_TAPER + 1));
}

/* [R16 items b/c] Which bank of a bridge gorge is the COASTLINE. A bridge run
 * lays a river plane BRIDGE_WATER_HALF either side of the deck, so the gorge has
 * water on BOTH banks, yet there is no biome sea here to read a side from -- this
 * seed has no coastal biome, so tg_ground_side's `water_side` is 0.0 everywhere
 * (which is exactly why the earlier tg_bridge_skirt_redundant sat inert). Pick
 * ONE bank per run to treat as the coast we keep clean, stable along the whole
 * run so it cannot flip mid-crossing, from the run's first span -- the same hash
 * shape tg_water_side uses for a biome coast. Returns +1 (left of travel) or -1;
 * 0 off a bridge run. The value is used as the seaward sign, so exactly one of
 * the two sides ever matches -- the property tg_emit_ground relies on to never
 * skip both halves of its single two-sided mesh. */
static double tg_bridge_coast_side(const TG_NodeList *nl, int si)
{
    int s0, s1;
    unsigned int h;
    if (!tg_span_in_bridge_run(si)) return 0.0;
    tg_bridge_run_bounds(nl, si, &s0, &s1);
    h = (unsigned)s0 * 2246822519u;
    return (h & 1u) ? 1.0 : -1.0;
}

static void tg_ground_side_raw(const TG_NodeList *nl, int si, int is_left,
                               double water_side, TG_GroundProf *p)
{
    const double phase = tg_bridge_gorge_phase(nl, si);
    const int seaward = (water_side > 0.0 && is_left) ||
                        (water_side < 0.0 && !is_left);

    /* Default: flush with the asphalt at the road edge, a gentle embankment
     * outward. The INNER point stays at road level on purpose -- dropping the
     * whole skirt left a thin void/lip between road and grass. */
    p->n = 2;
    p->d[0]  = 0.0;                    p->dy[0] = 0.0;
    p->d[1]  = tg_verge_reach();      p->dy[1] = TD5_TG_GROUND_DROP;

    /* [R16 item c] STEEP COASTLINE on the bridge's coast side.
     *
     * Reported, on several consecutive bridge-run spans: "this should be an
     * actual coastline with a steep slope perpendicular to the road ... the way
     * it is getting sloped to the road looks wrong."
     *
     * The gorge bank (below) starts at phase*GORGE_INSET out from the road edge
     * and ramps its inner point from road level (near the mouths, phase small) to
     * submerged (at the crown). Near the mouths that inner point sits at road
     * level a short way out, so the bank reads as ground gently SLOPING INTO THE
     * ROAD rather than as a shore. A coast meets the water as a bank perpendicular
     * to the road: drop from the road edge straight down to the water surface
     * within one shore-verge width, then it is under the river plane.
     *
     * Coast side only (tg_bridge_coast_side, +1/-1, never both), so the opposite
     * bank keeps the tuned gorge profile the earlier rounds shaped, and the change
     * is confined to the one bank the user is standing beside. The bank descends
     * to tg_bridge_water_surf_y -- the SAME surface accessor the river plane, the
     * coast band and the R14 wrap read -- so it lands exactly on the water it
     * meets, and because tg_topo_chain is built from this profile the R14 tree-line
     * wrap (item d) follows the new shore automatically instead of the old ramp.
     * TD5RE_R16_BRIDGE_COAST_SLOPE=0 restores the gorge ramp for an A/B. */
    if (td5_env_flag_on("TD5RE_R16_BRIDGE_COAST_SLOPE")
        && tg_span_in_bridge_run(si) && tg_water_span_clear(si)) {
        const double cs = tg_bridge_coast_side(nl, si);
        if ((cs > 0.0 && is_left) || (cs < 0.0 && !is_left)) {
            double drop = nl->v[si].y - tg_bridge_water_surf_y(nl, si);
            if (drop < TD5_TG_GROUND_DROP) drop = TD5_TG_GROUND_DROP;
            p->n = 2;
            p->d[0] = 0.0;                p->dy[0] = 0.0;
            p->d[1] = TD5_TG_SHORE_VERGE; p->dy[1] = drop;
            return;
        }
    }

    /* [R17 WATER item 4] "this grass is wrongly placed ... there's no sidewalk
     * and it's over water."
     *
     * At the two ENDS of a bridge water run phase is exactly 0, so neither the
     * gorge pull-back (phase > 0, below) nor the seaward beach (needs a biome
     * water_side, which is 0 on a seed with no coastal biome) fires. The side
     * then falls through to the default flat verge -- a flat skirt sitting at
     * road level directly over the river plane, with no sidewalk. That is the
     * reported grass-over-water. The R16 coast-slope block above already turns
     * the COAST bank into a shore at every phase; do the same for the OTHER bank
     * at the run mouths, dropping it straight to the water surface so it meets
     * the water instead of floating over it. Scoped to phase <= 0 so the tuned
     * mid-run gorge/submerge profile (phase > 0) is untouched. `seaward` is
     * excluded so a real biome coast still uses its own tuned beach below.
     * TD5RE_R17_WATER_SKIRT_SHORE=0 restores the flat verge for an A/B. */
    if (td5_env_flag_on("TD5RE_R17_WATER_SKIRT_SHORE")
        && tg_span_in_bridge_run(si) && tg_water_span_clear(si)
        && phase <= 0.0 && !seaward) {
        double drop = nl->v[si].y - tg_bridge_water_surf_y(nl, si);
        if (drop < TD5_TG_GROUND_DROP) drop = TD5_TG_GROUND_DROP;
        p->n = 2;
        p->d[0] = 0.0;                p->dy[0] = 0.0;
        p->d[1] = TD5_TG_SHORE_VERGE; p->dy[1] = drop;
        return;
    }

    /* [R4 item 16a] The GORGE wins over the seaward beach on a bridge run.
     * On a COAST bridge (seed 99991 span 1160-1199) the seaward test fired first
     * and laid a FLAT beach verge alongside the raised deck -- a light concrete
     * strip hanging at deck height, which reads as "a bridge with a sidewalk on
     * the left". A raised deck crosses the water; there is no beach beside it, so
     * inside the run (phase > 0) the bank must pull back and drop to the river on
     * BOTH sides. phase is 0 exactly at the run ends, so the beach still applies
     * there and the two treatments stay continuous at the boundary. */
    if (seaward && phase <= 0.0) {
        const double d = nl->v[si].y - tg_sea_level_y(nl, si);
        const double fall = (d > 0.0 ? d : (double)TD5_TG_WATER_DROP)
                          - TD5_TG_GROUND_DROP;
        p->n = 3;
        p->d[1]  = TD5_TG_SHORE_VERGE;  p->dy[1] = TD5_TG_GROUND_DROP;
        p->d[2]  = TD5_TG_SHORE_END;
        p->dy[2] = TD5_TG_GROUND_DROP
                 + fall * (TD5_TG_SHORE_END - TD5_TG_SHORE_VERGE)
                        / ((double)TD5_TG_WATER_BEACH - TD5_TG_SHORE_VERGE);
        return;
    }
    if (phase > 0.0) {
        /* Gorge: the bank pulls back from the deck; the pull-back scales with the
         * run phase so the terrain is continuous with the ordinary skirt at the
         * run ends. `bed` is the R6 drop target (just under the river bed); it is
         * clamped non-negative because a river plane above its road node is not a
         * gorge and a negative drop would lift the terrain above the road. */
        double bed = nl->v[si].y - (tg_bridge_water_y(nl, si) - 150.0);
        if (bed < 0.0) bed = 0.0;
        p->d[0]  = phase * TD5_TG_GORGE_INSET;
        /* [R6 item 14] The OUTER edge used to stay at the ordinary 24000 verge,
         * so once the inner point dropped, the whole skirt was a wide near-flat
         * GROUND (concrete-tile) shelf lying ACROSS the river. Bring the outer
         * edge in to a narrow band at the crown, lerping back to the ordinary
         * verge at the run ends so it stays continuous with the plain skirt.
         *
         * [R8 item 11] "Back to the ordinary verge" was written as the literal
         * 24000, and R6 CITY item 6 then narrowed the ordinary verge to 12000
         * without this line hearing about it. MEASURED (seed 99991, R8BDIAG):
         * span 1000 outer edge 12000, span 1001 outer edge 23864 -- an 11864
         * lateral jump between two adjacent slabs at the run mouth. Take the
         * base from tg_verge_reach() so the gorge lands on whatever the plain
         * skirt actually reaches, by construction rather than by coincidence. */
        p->d[1]  = tg_verge_reach()
                 + phase * (phase * TD5_TG_GORGE_INSET + 3000.0
                            - tg_verge_reach());
        /* [R7 item 17] "Below bridges only water should be rendered", still not
         * true after R6. R6 dropped the skirt to the river BED (wy-150) but ramped
         * there LINEARLY with phase, so across the whole INTERIOR of the run the
         * concrete skirt sat ABOVE the water surface (wy+100) -- the "tiles at one
         * height and water at a lower height on alternated spans" the user still
         * sees. The bed was never the right target: the bridge-water plane
         * (BRIDGE_WATER_HALF 32000) already covers the entire gorge, so anything
         * at or above the water surface pokes through it. Drop the skirt just
         * BELOW the water surface on a FAST ramp (fully submerged by ~30% into the
         * run), so the interior shows only water while the run ENDS (phase -> 0)
         * still rise to the bank as a shore. Both profile points share the depth,
         * so the submerged strip is flat and wholly hidden by the water plane.
         * TD5RE_AUTOTRACK_BRIDGE_SUBMERGE=0 restores the R6 bed ramp for an A/B. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_SUBMERGE")) {
            /* [R8 item 11] R7's ramp was applied to BOTH profile points at once,
             * so the bank was a FLAT SHELF whose single height ramped from road
             * level to below the water and back. A flat shelf is above the water
             * for every span where the ramp has not finished, and the ramp is
             * symmetric in phase, so it is unfinished at BOTH ends of every run.
             *
             * MEASURED (seed 99991, run 1000-1039, R8BDIAG skirt vs surf):
             * spans 1000-1007 and 1033-1039 -- 15 of 40 -- carried the shelf
             * ABOVE the river surface, by up to 4782 at the mouth and 754 at
             * span 1033. Span 1031 is where the user is standing when they
             * report "alternation between water spans and tile spans at
             * different height", and it is two spans before the shelf surfaces.
             *
             * The shelf is the wrong SHAPE, not the wrong height. A bank meets
             * water by descending THROUGH the surface -- the rule
             * TD5_TG_SHORE_VERGE/SHORE_END already encodes for the sea. So make
             * the gorge a shore too: the INNER point carries the phase ramp (it
             * is the bank top, at road level near the mouths and submerged at
             * the crown), and everything beyond one bank-face width is pinned
             * FULLY SUBMERGED regardless of phase. Then no span can show a flat
             * horizontal slab above the water: the only ground above the surface
             * is the sloping bank face, which is what a shore looks like. */
            const double surf = tg_bridge_water_surf_y(nl, si);
            double sub = nl->v[si].y - surf + 250.0;   /* 250 below the surface */
            double r   = phase * (1.0 / 0.30);
            double face;
            if (sub < TD5_TG_GROUND_DROP) sub = TD5_TG_GROUND_DROP;
            if (r > 1.0) r = 1.0;
            p->dy[0] = TD5_TG_GROUND_DROP + r * (sub - TD5_TG_GROUND_DROP);
            /* Bank face: wide enough that the descent is a slope and not a
             * cliff, and short enough that it is over well inside the river
             * half-width (32000) at every phase. */
            face = TD5_TG_SHORE_END - TD5_TG_SHORE_VERGE;   /* 6000 */
            if (!tg_r8_bridge_water()) {
                p->dy[1] = p->dy[0];        /* R7 flat shelf, for the A/B */
            } else if (p->d[0] + face < p->d[1]) {
                p->n     = 3;
                p->d[2]  = p->d[1];
                p->dy[2] = sub;
                p->d[1]  = p->d[0] + face;
                p->dy[1] = sub;
            } else {
                p->dy[1] = sub;      /* no room for a face: outer point is bed */
            }
        } else {
            p->dy[0] = phase * bed;
            p->dy[1] = TD5_TG_GROUND_DROP + phase * (bed - TD5_TG_GROUND_DROP);
        }
        return;
    }
    if (!is_left) {
        /* Branch corridor bows into the right verge -- keep off its carriageway. */
        p->d[0] = tg_ground_branch_clear(nl, si);
        /* [R8 SHAPE G5] ...but the skirt still has to END somewhere OUTBOARD of
         * the corridor, and the clamp below is what decides that. Before R8 the
         * widest a corridor ever reached (bow 1.20) was ~7400 past the road edge
         * and the 12000 verge covered it. A long fork bows past 18000, so the
         * clamp would drag the skirt's INNER point back to 11000 and lay a
         * 1000-wide sliver of ground in the open air between the two
         * carriageways -- with the branch itself, and everything beyond it,
         * standing on nothing. So push the OUTER point out to clear the corridor
         * first and keep an apron beyond it.
         *
         * The apron is deliberately narrower than the ordinary 12000 verge: a
         * wide near-flat skirt on a descent projects over the road ahead and
         * hides it (the R6 item-6 finding that cut the verge from 24000), and
         * this one sits on the far side of the branch where it would hide the
         * CORRIDOR. 4000 reaches past the branch's own pavement without
         * becoming that apron again. */
        if (tg_r8_longbranch_enabled()) {
            const double need = p->d[0] + TD5_TG_R8_LONG_APRON;
            if (need > p->d[1]) p->d[1] = need;
        }
        if (p->d[0] > p->d[1] - 1000.0)
            p->d[0] = p->d[1] - 1000.0;
    }

    /* [R14 OVERPASS item 3] HOLD THE FLOOR FLAT UNDER A CROSSING. Applied LAST
     * in the ordinary branch and as a max(), so it can only ever push the outer
     * point further out: the branch-corridor clearance above and the widened
     * long-fork apron keep whatever they decided, and the seaward beach and the
     * gorge (which returned above) are untouched -- a crossing never lands on
     * either, tg_up_span_crossable refuses both outright.
     *
     * The drop at the outer point is left at TD5_TG_GROUND_DROP, which is what
     * makes this a floor rather than a ramp: the skirt stays near road level all
     * the way across the deck's footprint, and the far band then tucks under its
     * new outer edge and sinks from THERE, off past the end of the deck. */
    {
        const double up = tg_r14_up_ground_reach(si);
        if (up > p->d[p->n - 1]) p->d[p->n - 1] = up;
    }
}

/* [R9 TOPO C3] Every consumer of the near cross-section -- the skirt slab, the
 * far band's seam, the flora planter -- goes through here, so the "another road
 * is closer than my ground reaches" rule cannot be honoured by one of them and
 * missed by the next. Purely a CLAMP on the raw profile: the outer point (and
 * any point beyond it) is pulled in to the neighbouring carriageway's edge and
 * its drop is interpolated along the segment it landed in, so the surface keeps
 * its shape and simply stops earlier. */
void tg_ground_side(const TG_NodeList *nl, int si, int is_left,
                           double water_side, TG_GroundProf *p)
{
    double cap;
    int k;

    tg_ground_side_raw(nl, si, is_left, water_side, p);
    /* [R13 JUNCTION item 3] BEND FOLD, applied in the same place and the same
     * way as the R9 topo cap: the outer point is pulled in, its drop
     * interpolated along the segment it landed in, and every consumer of the
     * cross-section -- the skirt slab, the far band's seam, the flora planter --
     * inherits it, so the backdrop comes in WITH the ground rather than leaving
     * a bare ring on the inside of a bend. Taken as a min with the topo cap
     * rather than as a separate pass, so there is still exactly one clamp. */
    cap = tg_r13_fold_cap(nl, si, is_left ? 1.0 : -1.0, 0.0, 1e30,
                          "TD5RE_R13_FOLD_GROUND");
    if (!tg_topo_enabled()) {
        if (cap >= p->d[p->n - 1]) return;
    } else {
        const double tcap = tg_topo_road_cap(nl, si, is_left);
        if (tcap < cap) cap = tcap;
        if (cap >= p->d[p->n - 1]) return;
    }
    if (cap < p->d[0] + TD5_TG_TOPO_GAP_TOL) cap = p->d[0] + TD5_TG_TOPO_GAP_TOL;
    for (k = 1; k < p->n; k++) {
        if (p->d[k] <= cap) continue;
        {
            const double span = p->d[k] - p->d[k - 1];
            const double t = (span > 1e-6) ? (cap - p->d[k - 1]) / span : 0.0;
            p->dy[k] = p->dy[k - 1] + t * (p->dy[k] - p->dy[k - 1]);
            p->d[k]  = cap;
        }
        p->n = k + 1;
        break;
    }
}

/* [SKIRT/COAST 2026-09-07] Is this side's ground skirt redundant because the
 * bridge's own water + coast already cover it?
 *
 * Reported, on two consecutive bridge spans (kind=skirt, page 5 GROUND):
 * "this geometry at the side of the bridge should be deleted if there's already
 * a coastline".
 *
 * tg_emit_ground is the unconditional first mesh of EVERY ring span. On a bridge
 * run it is reshaped -- the gorge pulls the bank back and (with SUBMERGE on)
 * drives it under the river surface -- but it is never dropped, and it never
 * consults tg_emit_bridge_coast or the water plane. Beside a coast bridge that
 * leaves a GROUND slab in the same place the coast band and the water already
 * describe, which is the doubled geometry that was picked.
 *
 * Deliberately narrow, so it cannot punch a hole in the terrain:
 *   - SEAWARD side only. `seaward` uses the identical expression as
 *     tg_ground_side_raw, and water_side is +1/-1 for the biome run, so at most
 *     ONE of the two sides can ever match. The landward skirt is untouched.
 *   - When water_side is 0.0 (no sea in this biome) `seaward` is false on both
 *     sides, so a non-coastal bridge keeps both skirts exactly as before.
 *   - Bridge runs only, and only where the water is actually clear there.
 * Knob TD5RE_BRIDGE_SKIRT_COAST=0 restores the old both-sides behaviour. */
static int tg_bridge_skirt_redundant(const TG_NodeList *nl, int si,
                                     int is_left, double water_side)
{
    double coast = water_side;
    /* [R16 item b] RE-KEY to the bridge's own coast signal so the suppression
     * actually fires. As written this helper keyed on the biome `water_side`,
     * which is +/-1 only where a biome carries a sea. On a seed with no coastal
     * biome (this one) it is 0 on both sides, so the seaward test never fired and
     * the whole helper was MEASURED byte-identical on or off. The water beside a
     * bridge deck is the run's own river, not a biome sea, so take the side from
     * tg_bridge_coast_side (+/-1 per run, so still exactly one side can match).
     * TD5RE_R16_BRIDGE_COAST_SIDE=0 keeps the biome-only key. */
    if (coast == 0.0 && nl && tg_span_in_bridge_run(si)
        && td5_env_flag_on("TD5RE_R16_BRIDGE_COAST_SIDE"))
        coast = tg_bridge_coast_side(nl, si);
    {
        const int seaward = (coast > 0.0 && is_left) ||
                            (coast < 0.0 && !is_left);
        if (!seaward)                    return 0;
    }
    if (!tg_span_in_bridge_run(si))      return 0;
    if (!tg_water_span_clear(si))        return 0;
    /* [R16 item b] Drop the coast-side skirt only at the run ENDS, where the
     * transverse bridge coast band (tg_emit_bridge_coast, fired at s0 and s1)
     * already caps the mouth. Mid-run the coast-side skirt is the steep shore
     * from item c -- the only geometry between the deck edge and the river on
     * that bank -- so dropping it there would open the bank face to the sky.
     * Only applies to the re-keyed bridge case (biome coast keeps run-wide).
     *
     * [R17 WATER item 3] "the geometry on the side of the road while there's a
     * bridge should be deleted (this is one span, it's alongside the WHOLE
     * bridge)." TD5RE_R17_BRIDGE_SKIRT_RUNWIDE lifts the end-only restriction so
     * the seaward skirt is dropped along the whole run. OFF BY DEFAULT and
     * UNVERIFIED (no game assets here): on a bridge run the far band is not
     * drawn, so the drawn world ends at the river plane; the coast-side skirt is
     * the only geometry closing the vertical face between the deck edge and the
     * water mid-run, and dropping it can open that face to the sky. Provided as
     * an A/B lever, not a default, precisely because it trades the redundant
     * shelf the user reported against that risk. */
    if (nl && water_side == 0.0
        && td5_env_flag_on("TD5RE_R16_BRIDGE_COAST_SIDE")
        && !td5_env_flag_off("TD5RE_R17_BRIDGE_SKIRT_RUNWIDE")) {
        int s0, s1;
        tg_bridge_run_bounds(nl, si, &s0, &s1);
        if (si != s0 && si != s1)        return 0;
    }
    if (!td5_env_flag_on("TD5RE_BRIDGE_SKIRT_COAST")) return 0;
    return 1;
}

/* [R16 item B 2026-09-07] Is this side's ground skirt hidden by an UNBROKEN city
 * frontage, so the grass under it can never be seen from the road?
 *
 * Reported (kind=skirt, page 2 GREEN, seed 20260907 e67 = spans 268-271): "this
 * grass is never visible". On a paved CITY span the skirt is covered end to end
 * on that side:
 *   - the NEAR strip [road edge .. sidewalk_w] lies under the RAISED PAVEMENT
 *     slab (tg_r12_pave_stands), whose kerb face closes the step to the asphalt;
 *   - the FAR strip [sidewalk_w .. verge] stands behind the FACADE WALL, whose
 *     base sits flush at the kerb (tg_side_geom starts the wall at KERB_H) and
 *     whose buildings run outward past the verge.
 * With both present there is opaque geometry over the whole skirt on that side.
 *
 * Modelled on tg_r13_band_covers (the far-band tree-wall cull in this file):
 * the wall must be UNBROKEN over the slab AND one span of margin either side,
 * because a single gap at the slab's end would expose the grass behind it to an
 * oblique view down the street. Uses the generator's own frontage predicate
 * (tg_facade_stands + tg_facade_built) exactly as the R13 apron/edge rules do,
 * so this cannot disagree with where the wall actually is.
 *
 * Deliberately narrow, and UNVERIFIED at runtime (this build has no game
 * assets), so it is gated OFF by default -- TD5RE_R16_CITY_SKIRT_CULL=1 to A/B
 * it. tg_facade_stands is 0 on bridge runs and under overpass decks, so this
 * never fires there; a tree-band biome has no sidewalk (tg_city_sidewalk_w==0)
 * so tg_facade_stands is 0 and the far-band cull keeps its own domain. */
static int tg_city_skirt_hidden(const TG_NodeList *nl, int si, int is_left)
{
    const int left = is_left ? 1 : 0;
    int k;
    if (!td5_env_flag_off("TD5RE_R16_CITY_SKIRT_CULL")) return 0;
    if (si <= 0 || si + 1 >= nl->count)                 return 0;
    /* NEAR strip: raised pavement on this side over both ends of the slab. */
    if (!tg_r12_pave_stands(nl, si,     left)) return 0;
    if (!tg_r12_pave_stands(nl, si + 1, left)) return 0;
    /* FAR strip: an unbroken frontage over the slab plus one span of margin. */
    for (k = si - 1; k <= si + 2; k++) {
        if (k < 0 || k >= nl->count)              return 0;
        if (!tg_facade_stands(k))                 return 0;
        if (!tg_facade_built(k, left))            return 0;
    }
    return 1;
}

int tg_emit_ground(const TG_NodeList *nl, int si, TG_Buf *blk,
                          double water_side)
{
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len;
    /* Up to (MAXPT-1) quads per side. */
    double px[(TD5_TG_GROUND_MAXPT - 1) * 8], py[(TD5_TG_GROUND_MAXPT - 1) * 8];
    double pz[(TD5_TG_GROUND_MAXPT - 1) * 8], uu[(TD5_TG_GROUND_MAXPT - 1) * 8];
    double vv[(TD5_TG_GROUND_MAXPT - 1) * 8];
    /* [R9 TOPO C4] The SURFACE material comes from the ground-run authority,
     * not from this span's dithered biome roll. See tg_topo_ground_index. */
    int seg_page = tg_topo_surface_page(si), seg_nq;
    int s, k, n = 0;
    /* [R16 item B] Drop a side's grass skirt where a city frontage hides it end
     * to end. Precomputed for both sides so that if BOTH would cull we keep the
     * left one: the mesh must never come out empty (the skirt is the span's
     * first/ground model, and a zero-vert mesh would shift every later model
     * index for the span). At most a wasted hidden slab on one side then. */
    int cull_l = tg_city_skirt_hidden(nl, si, 1);
    int cull_r = tg_city_skirt_hidden(nl, si, 0);
    /* [R16 MERGE 2026-09-07] There are now TWO independent per-side culls in the
     * slab loop below: the bridge/coast one (tg_bridge_skirt_redundant -- seaward
     * side of a bridge run) and the city one (tg_city_skirt_hidden -- a side
     * hidden end to end behind a frontage). Each was written on its own branch
     * guaranteeing "at most ONE side can match", and each is correct in
     * isolation, but neither could see the other. Together they can take one side
     * EACH: the bridge cull drops the seaward half while the city cull drops the
     * landward half, both `continue`, and tg_write_quad_mesh is handed n == 0.
     * That is the empty-mesh case both branches were trying to avoid -- the skirt
     * is the span's first/ground model, so a zero-vert mesh shifts every later
     * model index for the span.
     * So resolve BOTH culls up front and, if between them they would empty the
     * mesh, keep the left slab (same tie-break the city cull already used). Cost
     * is at most one wasted hidden slab; the alternative is corrupt indices. */
    int skip_l = cull_l || tg_bridge_skirt_redundant(nl, si, 1, water_side);
    int skip_r = cull_r || tg_bridge_skirt_redundant(nl, si, 0, water_side);
    if (skip_l && skip_r) skip_l = 0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

    /* Outward direction = along the cross-section, away from the centre. */
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    /* One quad per profile segment per side, loop order near-in, near-out,
     * far-out, far-in so each quad is a proper ring.
     *
     * ISOTROPIC UV: V advances one tile per span (~SPAN_LENGTH world units), so U
     * is the OUTWARD DISTANCE in span-lengths, which makes each tile square. U
     * running 0..4 across the whole 24000-unit skirt was a ~4:1 lateral stretch
     * that smeared the texture and defeated the (isotropic, box-filter) mipmaps,
     * so the far ground shimmered. Taking U from the distance also means the
     * tiling does not change when the profile does. */
    for (s = 0; s < 2; s++) {
        const int is_left = s ? 0 : 1;
        const double ox = is_left ? nux : -nux, oz = is_left ? nuz : -nuz;
        const double gx = is_left ? fux : -fux, gz = is_left ? fuz : -fuz;
        const double bnx = is_left ? nlx : nrx, bnz = is_left ? nlz : nrz;
        const double bfx = is_left ? flx : frx, bfz = is_left ? flz : frz;
        const double bny = is_left ? nly : nry, bfy = is_left ? fly : fry;
        TG_GroundProf pa, pb;
        int nseg;

        /* [R8 item 11] TWO profiles, one per END of the slab.
         *
         * This function used to evaluate the profile ONCE, at span si, and then
         * use it for the slab's near edge (node si) AND its far edge (node
         * si+1). Off a bridge run that is invisible because the profile is a
         * constant, so the seam between consecutive slabs closes by accident.
         * Inside a bridge run the profile is a steep function of the gorge
         * phase, so slab(si)'s far edge was built from profile(si) while
         * slab(si+1)'s near edge was built from profile(si+1) -- two different
         * heights and two different lateral extents at the SAME node.
         *
         * MEASURED (seed 99991, run 1000-1039, R8BDIAG): the bank height moves
         * up to 990 units and the inner pull-back up to 700 units from one span
         * to the next near the run ends, so every span boundary there was a
         * vertical crack of that size with the river visible through it. That is
         * the per-span "alternation between water spans and tile spans at
         * different height", and it is why a frame taken at the crown (where the
         * profile is flat and the seams close) passed in rounds 6 and 7 while
         * the complaint span kept coming back.
         *
         * Sampling both ends makes the slab a proper ruled surface between the
         * two cross-sections: slab(si)'s far edge and slab(si+1)'s near edge are
         * now the same profile evaluated at the same node, so they agree by
         * construction at every span, on any profile, forever. Where the two
         * ends have different point counts (the boundary between the seaward
         * beach and the gorge) the index is clamped, which pairs the last real
         * point with itself and closes the seam with a degenerate quad rather
         * than a hole. */
        /* [R16 MERGE] Both per-side culls, already reconciled above so at least
         * one slab always survives: the bridge/coast seaward drop and the city
         * hidden-frontage drop. See the skip_l/skip_r note at the top. */
        if (is_left ? skip_l : skip_r) continue;

        tg_ground_side(nl, si, is_left, water_side, &pa);
        if (tg_r8_bridge_water() && si + 1 < nl->count)
            tg_ground_side(nl, si + 1, is_left, water_side, &pb);
        else
            pb = pa;                      /* R7 single-profile slab, for the A/B */
        nseg = (pa.n > pb.n ? pa.n : pb.n) - 1;
        for (k = 0; k < nseg; k++) {
            const int ka0 = (k     < pa.n) ? k     : pa.n - 1;
            const int ka1 = (k + 1 < pa.n) ? k + 1 : pa.n - 1;
            const int kb0 = (k     < pb.n) ? k     : pb.n - 1;
            const int kb1 = (k + 1 < pb.n) ? k + 1 : pb.n - 1;
            const double a0 = pa.d[ka0],  a1 = pa.d[ka1];
            const double b0 = pb.d[kb0],  b1 = pb.d[kb1];
            const double ay0 = pa.dy[ka0], ay1 = pa.dy[ka1];
            const double by0 = pb.dy[kb0], by1 = pb.dy[kb1];
            px[n]=bnx+ox*a0; py[n]=bny-ay0; pz[n]=bnz+oz*a0;
            uu[n]=a0/(double)TD5_TG_SPAN_LENGTH; vv[n]=(double)si;     n++;
            px[n]=bnx+ox*a1; py[n]=bny-ay1; pz[n]=bnz+oz*a1;
            uu[n]=a1/(double)TD5_TG_SPAN_LENGTH; vv[n]=(double)si;     n++;
            px[n]=bfx+gx*b1; py[n]=bfy-by1; pz[n]=bfz+gz*b1;
            uu[n]=b1/(double)TD5_TG_SPAN_LENGTH; vv[n]=(double)si+1.0; n++;
            px[n]=bfx+gx*b0; py[n]=bfy-by0; pz[n]=bfz+gz*b0;
            uu[n]=b0/(double)TD5_TG_SPAN_LENGTH; vv[n]=(double)si+1.0; n++;
        }
    }

    seg_nq = n / 4;
    tg_acct(TG_ACCT_TERRAIN, si);      /* one slab covering both verges */
    /* [R14 OVERPASS item 3] The widened floor changes THIS slab rather than
     * adding one, so its accounting run is recorded where the slab is written.
     * Without it the fix would be invisible to the element inventory and the
     * only proof it fired would be a frame, which is what the round is trying
     * not to argue from. */
    if (tg_r14_up_ground_reach(si) > 0.0) tg_acct(TG_ACCT_R14_UP, si);
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1);
}

static int tg_r12_fcross_on(void)
{
    return td5_env_flag_on("TD5RE_R12_FOREST_CROSS");   /* default ON */
}

static int tg_r12_fcross_forest(int si)
{
    return !strcmp(k_biomes[tg_scenery_biome_index(si)].name, "FOREST");
}

/* First span of the candidate street in block `blk`. The offset stops
 * TD5_TG_R12_FCROSS_WIDTH short of the block end so a street never straddles two
 * blocks -- one block, at most one candidate, no interaction between them. */
static int tg_r12_fcross_start(int blk)
{
    const unsigned int h = (unsigned)blk * 2654435761u + 0x9E3779B9u;
    const int room = TD5_TG_R12_FCROSS_PERIOD - TD5_TG_R12_FCROSS_WIDTH;
    return blk * TD5_TG_R12_FCROSS_PERIOD + (int)(h % (unsigned)room);
}

/* Clamped outward reach for the mouth at (si, sg): the R8 street rule with no
 * floor. Returns 0 when the ray cannot get clear of the road corridor at all. */
static double tg_r12_fcross_reach(const TG_NodeList *nl, int si, double sg)
{
    double e[10], d;
    int lo, hi;

    tg_city_edge_frame(nl, si, sg, e);
    lo = si - TD5_TG_R12_FCROSS_WIN; if (lo < 0) lo = 0;
    hi = si + TD5_TG_R12_FCROSS_WIN;
    if (hi > nl->count - 1) hi = nl->count - 1;

    for (d = TD5_TG_R12_FCROSS_CMIN; d <= TD5_TG_R12_FCROSS_WANT;
         d += TD5_TG_R12_FCROSS_STEP) {
        const double px = e[0] + e[6] * d, pz = e[2] + e[7] * d;
        double lat, best = 1e300;
        int i, ni = -1;
        for (i = lo; i <= hi; i++) {
            double dx, dz, d2;
            if (i > si - TD5_TG_R12_FCROSS_SKIP &&
                i < si + TD5_TG_R12_FCROSS_SKIP) continue;
            dx = px - nl->v[i].x; dz = pz - nl->v[i].z;
            d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; ni = i; }
        }
        if (ni < 0) break;
        lat = (px - nl->v[ni].x) * nl->v[ni].tz
            - (pz - nl->v[ni].z) * nl->v[ni].tx;
        {
            const double lim = tg_carriageway_reach(nl, ni,
                                   (lat >= 0.0) ? 1.0 : -1.0)
                             + TD5_TG_R12_FCROSS_MARGIN;
            if ((lat < 0.0 ? -lat : lat) < lim)
                return d - TD5_TG_R12_FCROSS_STEP;   /* last sample known clear */
        }
    }
    return TD5_TG_R12_FCROSS_WANT;
}

/* Is span si part of a forest side road? Writes the side (+1 = left of travel)
 * and the reach both spans agree on. Cheap gates first: everything but the two
 * candidate spans per block is rejected before the clamp runs. */
int tg_r12_fcross_at(const TG_NodeList *nl, int si,
                            double *pside, double *preach)
{
    int c, j;
    double side, reach = TD5_TG_R12_FCROSS_WANT;

    if (!tg_r12_fcross_on() || !nl || si <= TD5_TG_R12_FCROSS_CLEAR) return 0;
    if (!tg_r12_fcross_forest(si)) return 0;
    c = tg_r12_fcross_start(si / TD5_TG_R12_FCROSS_PERIOD);
    if (si < c || si >= c + TD5_TG_R12_FCROSS_WIDTH) return 0;
    /* SIDE from the same block hash, so both spans of one street agree. */
    side = ((unsigned)(si / TD5_TG_R12_FCROSS_PERIOD) * 2654435761u
            + 0x9E3779B9u) & 0x10000u ? 1.0 : -1.0;
    /* One span of shoulder either side of the street is checked too: the fold
     * walls stand on the street's outer nodes, which belong to c-1 and c+W. */
    for (j = c - 1; j <= c + TD5_TG_R12_FCROSS_WIDTH; j++) {
        double r;
        if (j <= TD5_TG_R12_FCROSS_CLEAR || j + 1 >= nl->count) return 0;
        if (!tg_r12_fcross_forest(j)) return 0;
        if (tg_span_in_tunnel(j) || tg_span_in_bridge_run(j)) return 0;
        if (tg_span_near_bridge(j, TD5_TG_XBRIDGE_CLEAR)) return 0;
        if (tg_branches_enabled() && tg_span_in_fork_clear(j)) return 0;
        if (tg_side_corridor_here(nl, j, side)) return 0;
        r = tg_r12_fcross_reach(nl, j, side);
        if (r < reach) reach = r;
    }
    /* LONG ENOUGH OR NOT AT ALL: a road that stops short of the tree wall is
     * the "cuts off abruptly" defect, so the candidate is dropped instead. */
    if (reach < TD5_TG_R12_FCROSS_MIN) return 0;
    if (pside)  *pside  = side;
    if (preach) *preach = reach;
    return 1;
}

/* [item 1a] Extra length added at EACH along-road edge so the measured mouth
 * reaches the two-lane goal. Zero when it already does. */
static double tg_r14_fcross_pad(const TG_NodeList *nl, int c)
{
    const int cf = c + TD5_TG_R12_FCROSS_WIDTH;
    double dx, dz, w, pad;

    if (!td5_env_flag_on("TD5RE_R14_FCROSS_WIDE")) return 0.0;
    if (c < 0 || cf >= nl->count) return 0.0;
    dx = nl->v[cf].x - nl->v[c].x;
    dz = nl->v[cf].z - nl->v[c].z;
    w  = sqrt(dx * dx + dz * dz);
    pad = (TD5_TG_R14_FCROSS_LANES * (double)TD5_TG_LANE_WIDTH - w) * 0.5;
    if (pad <= 0.0) return 0.0;                       /* already two lanes */
    if (pad > TD5_TG_R14_FCROSS_PAD_MAX) pad = TD5_TG_R14_FCROSS_PAD_MAX;
    return pad;
}

/* [item 1b] Surface page for the crossing that STARTS at span c. */
static int tg_r14_fcross_page(int c)
{
    const int blk = c / TD5_TG_R12_FCROSS_PERIOD;
    const unsigned int h = (unsigned)blk * 0x27220A95u ^ 0x5BD1E995u;

    if (td5_env_flag_on("TD5RE_R14_FCROSS_WORN")
        && ((h >> 12) & 0xFFu) < (unsigned)TD5_TG_R14_FCROSS_WORN_P)
        return tg_road_slot(k_road_surf[RS_DIRT].page_var);
    return td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_MARKINGS")
           ? (TD5_TG_PAGE_R4_CROSS + 0) : tg_road_page(c);
}

/* [item 1c] Is a forest crossing lying on (si, side), counting one span either
 * side? A prop is a footprint, not a point: a billboard planted on the shoulder
 * span still hangs over the mouth. */
int tg_r14_fcross_clear_side(const TG_NodeList *nl, int si, double side)
{
    int j;
    if (!td5_env_flag_on("TD5RE_R14_FCROSS_CLEAR")) {
        double fs = 0.0, fr = 0.0;      /* R12 behaviour: this span only */
        return tg_r12_fcross_at(nl, si, &fs, &fr) && fs == side;
    }
    for (j = si - 1; j <= si + 1; j++) {
        double fs = 0.0, fr = 0.0;
        if (j < 0) continue;
        if (tg_r12_fcross_at(nl, j, &fs, &fr) && fs == side) {
            s_r14_fcross_side_hit++;
            return 1;
        }
    }
    return 0;
}

/* [item 1c] The same question in the placement frame: `inner_d` is the nearest
 * edge of a footprint measured out from the MAIN road edge. */
int tg_r14_fcross_occupies(const TG_NodeList *nl, int si, double side,
                                  double inner_d)
{
    int j;
    if (!td5_env_flag_on("TD5RE_R14_FCROSS_CLEAR")) return 0;
    for (j = si - 1; j <= si + 1; j++) {
        double fs = 0.0, fr = 0.0;
        if (j < 0) continue;
        if (tg_r12_fcross_at(nl, j, &fs, &fr) && fs == side
            && inner_d < fr) return 1;
    }
    return 0;
}

/* [item 1d] Must the pavement run END on this (span, side)? Exactly the crossing
 * spans -- one span either way would leave a 1500-raw hole in the verge beside
 * the mouth, which is a second defect, not a fix. */
int tg_r14_fcross_pave_stop(const TG_NodeList *nl, int si, double side)
{
    double fs = 0.0, fr = 0.0;
    if (!td5_env_flag_on("TD5RE_R14_FCROSS_PAVESTOP")) return 0;
    if (!tg_r12_fcross_at(nl, si, &fs, &fr) || fs != side) return 0;
    s_r14_fcross_pave_stop++;
    return 1;
}

/* The carriageway: one quad per crossing span, kerb outward, no skew (a straight
 * lane is the readable shape -- see item 13). Purely OUTWARD from the road edge,
 * so the main carriageway is untouched. */
static int tg_r12_fcross_emit_road(const TG_FBHook *h, double sg, double reach)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double e[10], q[12], t[8];
    /* [R14 items 1a/1b] one page decision and one width correction, both taken
     * from the street's FIRST span so the two quads of one street agree. */
    const int c0 = tg_r12_fcross_start(h->si / TD5_TG_R12_FCROSS_PERIOD);
    const double pad = tg_r14_fcross_pad(h->nl, c0);
    const double lift = TD5_TG_VERGE_LIFT
                      + (pad > 0.0 ? TD5_TG_R14_FCROSS_LIFT : 0.0);
    int seg_page = tg_r14_fcross_page(c0);
    int seg_nq = 1, n = 0;
    const double drop = tg_xstreet_drop(reach);
    const double u_r  = reach / (double)TD5_TG_LANE_WIDTH;

    if (*h->nmesh >= h->maxmesh) return 1;
    tg_city_edge_frame(h->nl, h->si, sg, e);
    /* The pad only moves the street's OUTER edges (the first span's near edge
     * and the last span's far edge), never the seam between the two quads. */
    if (pad > 0.0) {
        if (h->si == c0) {
            const TG_Node *n0 = &h->nl->v[h->si];
            e[0] -= n0->tx * pad; e[2] -= n0->tz * pad;
        }
        if (h->si == c0 + TD5_TG_R12_FCROSS_WIDTH - 1) {
            const TG_Node *n1 = &h->nl->v[h->si + 1];
            e[3] += n1->tx * pad; e[5] += n1->tz * pad;
        }
    }
    q[0]  = e[0];                q[1]  = e[1] + lift;
    q[2]  = e[2];
    q[3]  = e[0] + e[6] * reach; q[4]  = e[1] + lift - drop;
    q[5]  = e[2] + e[7] * reach;
    q[6]  = e[3] + e[8] * reach; q[7]  = e[4] + lift - drop;
    q[8]  = e[5] + e[9] * reach;
    q[9]  = e[3];                q[10] = e[4] + lift;
    q[11] = e[5];
    t[0] = 0.0; t[1] = (double)h->si;
    t[2] = u_r; t[3] = (double)h->si;
    t[4] = u_r; t[5] = (double)h->si + 1.0;
    t[6] = 0.0; t[7] = (double)h->si + 1.0;
    tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);

    tg_acct(TG_ACCT_R12_CROSS, h->si);          /* forest carriageway quad */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                             &seg_page, &seg_nq, 1);
}

/* The FOLD: the two cut ends of the tree wall turned to run outward along the
 * street's own edges. Emitted once, on the street's FIRST span, as one mesh --
 * the two walls face each other a street apart, so a single bounding sphere is
 * still tight. Each wall is wound twice (front and back) because the corridor is
 * seen from the racing line on the way in and from the far end on the way out. */
static int tg_r12_fcross_emit_fold(const TG_FBHook *h, double sg, double reach)
{
    const TG_NodeList *nl = h->nl;
    const int c = h->si, cf = c + TD5_TG_R12_FCROSS_WIDTH - 1;
    const double band = tg_treeline_height(h->b);
    const double back = tg_treeline_back(h->b);
    double d0 = back - TD5_TG_R12_FOLD_LEAD;
    double px[16], py[16], pz[16], uu[16], vv[16];
    double en[10], ef[10];
    /* One page, but the writer wants a per-QUAD segment list. */
    int seg_page[4], seg_nq[4], nseg = 0, n = 0, w;

    if (!(band > 0.0)) return 1;
    if (cf + 1 >= nl->count) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    if (d0 < 0.0) d0 = 0.0;
    if (reach <= d0 + 1.0) return 1;

    tg_city_edge_frame(nl, c,  sg, en);
    tg_city_edge_frame(nl, cf, sg, ef);
    /* [R14 item 1a] The fold walls ARE the street's two edges, so they take the
     * same pad the carriageway does or the corridor stops matching its road. */
    {
        const double pad = tg_r14_fcross_pad(nl, c);
        if (pad > 0.0) {
            en[0] -= nl->v[c].tx * pad;      en[2] -= nl->v[c].tz * pad;
            ef[3] += nl->v[cf + 1].tx * pad; ef[5] += nl->v[cf + 1].tz * pad;
        }
    }

    for (w = 0; w < 2; w++) {
        /* w 0 = the wall on the street's NEAR along-road edge (node c),
         * w 1 = the wall on its FAR edge (node cf+1). */
        const double ox = w ? ef[8] : en[6], oz = w ? ef[9] : en[7];
        const double bx = w ? ef[3] : en[0], bz = w ? ef[5] : en[2];
        const double by = nl->v[w ? cf + 1 : c].y - TD5_TG_TREELINE_SINK;
        const double x0 = bx + ox * d0,    z0 = bz + oz * d0;
        const double x1 = bx + ox * reach, z1 = bz + oz * reach;
        /* Square tiles, the same rule the band it continues uses: one tile is
         * `band` wide, so the page is never stretched along the corridor. */
        const double uw = (reach - d0) / band;
        const double vt = TD5_TG_FACADE_UV_INSET;
        const double vb = 1.0 - TD5_TG_FACADE_UV_INSET;
        int f;
        for (f = 0; f < 2; f++) {           /* both windings */
            double q[12], t[8];
            if (n + 4 > 16 || nseg >= 4) break;
            if (!f) {
                q[0]=x0; q[1]=by;        q[2]=z0;
                q[3]=x1; q[4]=by;        q[5]=z1;
                q[6]=x1; q[7]=by + band; q[8]=z1;
                q[9]=x0; q[10]=by + band; q[11]=z0;
                t[0]=0.0; t[1]=vb; t[2]=uw; t[3]=vb;
                t[4]=uw;  t[5]=vt; t[6]=0.0; t[7]=vt;
            } else {
                q[0]=x0; q[1]=by + band; q[2]=z0;
                q[3]=x1; q[4]=by + band; q[5]=z1;
                q[6]=x1; q[7]=by;        q[8]=z1;
                q[9]=x0; q[10]=by;        q[11]=z0;
                t[0]=0.0; t[1]=vt; t[2]=uw; t[3]=vt;
                t[4]=uw;  t[5]=vb; t[6]=0.0; t[7]=vb;
            }
            tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            seg_page[nseg] = TD5_TG_PAGE_TREELINE;
            seg_nq[nseg] = 1;
            nseg++;
        }
    }
    if (n <= 0) return 1;
    tg_acct_n(TG_ACCT_R12_CROSS, c, nseg);      /* fold wall quads */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                             seg_page, seg_nq, nseg);
}

/* Group R12-CROSS dispatcher. Marks its two halves SEPARATELY -- the enclosing
 * mark in the scenery loop is wider, and tg_guard_kind_of takes the NARROWEST
 * matching mark, so these win. */
int tg_emit_fb_forest_cross(const TG_FBHook *h)
{
    double sg = 1.0, reach = 0.0;
    size_t m0;

    if (!tg_r12_fcross_at(h->nl, h->si, &sg, &reach)) return 1;

    m0 = h->blk->len;
    if (!tg_r12_fcross_emit_road(h, sg, reach)) return 0;
    tg_guard_mark(m0, h->blk->len, TG_GK_ROAD, h->si);

    /* The fold belongs to the STREET, not to a span, so it is laid once. */
    if (tg_r12_fcross_start(h->si / TD5_TG_R12_FCROSS_PERIOD) == h->si) {
        m0 = h->blk->len;
        if (!tg_r12_fcross_emit_fold(h, sg, reach)) return 0;
        tg_guard_mark(m0, h->blk->len, TG_GK_FLORA, h->si);
    }
    return 1;
}

/* [R12 CROSS item 5] THE INSTRUMENT. Every candidate block, why it was accepted
 * or rejected, so "how many forest crossings does this seed carry" is a number
 * in race.log rather than a frame. TD5RE_R12_FCROSS_DIAG=1. */
void tg_r12_fcross_report(const TG_NodeList *nl, int nspans)
{
    int blk, kept = 0, cand = 0;
    if (!td5_env_flag_off("TD5RE_R12_FCROSS_DIAG")) return;
    for (blk = 0; blk * TD5_TG_R12_FCROSS_PERIOD < nspans; blk++) {
        const int c = tg_r12_fcross_start(blk);
        double sg = 1.0, reach = 0.0;
        if (c + TD5_TG_R12_FCROSS_WIDTH >= nspans) continue;
        cand++;
        if (tg_r12_fcross_at(nl, c, &sg, &reach)) {
            kept++;
            {   /* [R14 FCROSS item 1a] MEASURED width, not the span count: the
                 * street's two along-road edges are nodes c and c+WIDTH, so the
                 * carriageway is the chord between them. Printed in LANES so
                 * "up to two lanes" is a number rather than an impression. */
                const int cf = c + TD5_TG_R12_FCROSS_WIDTH;
                const double dx = nl->v[cf].x - nl->v[c].x;
                const double dz = nl->v[cf].z - nl->v[c].z;
                const double w   = sqrt(dx * dx + dz * dz);
                const double pad = tg_r14_fcross_pad(nl, c);
                const double we  = w + 2.0 * pad;
                const int page   = tg_r14_fcross_page(c);
                const int worn   = (page == tg_road_slot(
                                        k_road_surf[RS_DIRT].page_var));
                if (worn) s_r14_fcross_worn++;
                TD5_LOG_I(LOG_TAG, "trackgen: [R12 FCROSS] block %2d span %4d "
                          "KEPT side=%s reach=%.0f (wall at %.0f) width=%.0f "
                          "(%.2f lanes) +pad=%.0f -> %.0f (%.2f lanes) "
                          "main=%.0f page=%d %s", blk, c,
                          sg > 0.0 ? "left" : "right", reach, 11000.0, w,
                          w / (double)TD5_TG_LANE_WIDTH, pad, we,
                          we / (double)TD5_TG_LANE_WIDTH, nl->v[c].width,
                          page, worn ? "WORN" : "marked");
            }
        } else {
            TD5_LOG_I(LOG_TAG, "trackgen: [R12 FCROSS] block %2d span %4d "
                      "dropped (forest=%d tunnel=%d bridge=%d fork=%d)", blk, c,
                      tg_r12_fcross_forest(c), tg_span_in_tunnel(c),
                      tg_span_in_bridge_run(c),
                      tg_branches_enabled() && tg_span_in_fork_clear(c));
        }
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [R12 FCROSS] forest crossings kept=%d of %d "
              "candidate blocks (knob TD5RE_R12_FOREST_CROSS=%s)",
              kept, cand, tg_r12_fcross_on() ? "on" : "off");
    /* [R14 FCROSS] The round's four items as four numbers, so "did it fire" is
     * never read off a frame. worn/kept is item 1b's rate; the rest are 1c/1d. */
    TD5_LOG_I(LOG_TAG, "trackgen: [R14 FCROSS] worn=%ld of %d kept (%.0f%%) "
              "props-refused=%ld animals-moved=%ld pavement-stops=%ld "
              "side-hits=%ld",
              s_r14_fcross_worn, kept,
              kept > 0 ? 100.0 * (double)s_r14_fcross_worn / (double)kept : 0.0,
              s_r14_fcross_prop_skip, s_r14_fcross_animal_moved,
              s_r14_fcross_pave_stop, s_r14_fcross_side_hit);
}

/* ==== [R13 BAND item 1a] "the first line of trees must START BEFORE the START
 * banner" ==================================================================
 *
 * CONFIRMED, not assumed. The START gantry is emitted by tg_emit_fb_track at
 * EXACTLY span TD5_TG_GRID_SPAN (24) -- `if (h->si != TD5_TG_GRID_SPAN &&
 * h->si != finish) return 1`. The band's own gate is `si <= TD5_TG_GRID_SPAN`,
 * so the first quad it lays is span 25. The wall therefore begins ONE SPAN
 * PAST the banner, which is the report verbatim: from the grid you look down a
 * bare corridor and the tree line switches on just after the gantry. The band
 * ledger (TD5RE_R12_FLORA_REPORT=1, TG_R12_BAND_GRID) prints the absent run as
 * spans 0..24 with reason "grid", which is the measurement.
 *
 * WHY THE GUARD EXISTS, AND WHY IT IS NOT SIMPLY DELETED. Every emitter in the
 * grid stretch carries some form of this gate, and for the ON-ROAD ones it is
 * load-bearing: the starting grid has to be clear of furniture the cars would
 * be parked inside. The band is not one of those. It stands
 * tg_treeline_back (11000) out from the road EDGE plus the fork clearance --
 * further out than any grid car, any gantry leg (GANTRY_OUT2 360) and the
 * ground skirt's own inner rings. tg_building_for_span already made exactly
 * this argument in an earlier round and narrowed its own copy of the gate to
 * `si <= 0`, noting "nothing here is ever ON the road ... it only needs
 * somewhere for the near cap to end". This is the same emitter class, so it
 * takes the same rule rather than losing its gate altogether: span 0 stays
 * clear (the wall needs a node pair and a near end to close against), spans
 * 1..24 gain the wall, and the tree line now runs THROUGH the banner instead
 * of starting after it.
 *
 * TD5RE_R13_BAND_GRID=0 restores the R12 band that starts at span 25. */
static int tg_r13_band_grid(void)
{
    return td5_env_flag_on("TD5RE_R13_BAND_GRID");    /* default ON */
}

/* First span the band may NOT stand on. */
static int tg_r13_band_first_span(void)
{
    return tg_r13_band_grid() ? 0 : TD5_TG_GRID_SPAN;
}

/* ==== [R13 BAND item 1b] "everything behind a line of trees is invisible to
 * the player, so there should not be anything there unless it's buildings" ===
 *
 * The single predicate for "does the tree-line wall stand on this (span,
 * side)?", mirroring tg_emit_fb_flora's own gate chain so the two cannot
 * disagree about where the wall is. Writes the wall's WORST case over the
 * quad -- the LOWEST of its two node heights (above the road, i.e. already net
 * of TD5_TG_TREELINE_SINK) and the FURTHEST of its two lateral setbacks
 * (measured from the road EDGE, the same origin the far band's own D[] uses,
 * and through tg_flora_gap_clear so a branch corridor that pulls the wall in
 * is reflected). Worst case in both, so an occlusion test built on it is
 * conservative at every point of the span.
 *
 * Returns 0 when TD5RE_R12_FLORA_BAND is off: the per-node height/setback pair
 * only exists on that path, and the R12-off band is the dithered one whose
 * presence is exactly what R12 item 9 called unreliable. No band model, no
 * culling -- the A/B stays single-variable. */
static int tg_r13_band_side(const TG_NodeList *nl, int si, double side,
                            double *out_top, double *out_lat)
{
    double bh[2], bk[2], fx_side = 0.0, fx_reach = 0.0, d0, d1;
    int sj;

    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TREELINE"))   return 0;
    if (!td5_env_flag_on("TD5RE_R12_FLORA_BAND"))       return 0;
    if (si <= tg_r13_band_first_span())                 return 0;
    if (si + 1 >= nl->count)                            return 0;
    if (tg_span_in_bridge_run(si))                      return 0;
    if (tg_r12_fcross_at(nl, si, &fx_side, &fx_reach) && fx_side == side)
        return 0;                                  /* the forest-crossing CUT */

    tg_r12_band_params(si,     &bh[0], &bk[0]);
    tg_r12_band_params(si + 1, &bh[1], &bk[1]);
    /* BOTH ends must carry wall. One tapering to zero is a sinking end, and a
     * sinking end hides nothing. */
    if (!(bh[0] > 0.0) || !(bh[1] > 0.0)) return 0;

    sj = (si + 2 < nl->count) ? si + 1 : si;
    d0 = tg_flora_gap_clear(nl, si, side, bk[0]);
    d1 = tg_flora_gap_clear(nl, sj, side, bk[1]);

    *out_lat = (d0 > d1) ? d0 : d1;
    *out_top = ((bh[0] < bh[1]) ? bh[0] : bh[1]) - TD5_TG_TREELINE_SINK;
    return (*out_top > 0.0);
}

/* Does an UNBROKEN wall stand on this side over the whole far-group, with a
 * whole group of margin either end? The margin is the reason this is not just
 * "is there a wall on my four spans": dropping a far band leaves the
 * NEIGHBOURING group's apron ending in an open cut edge, so the drop has to sit
 * at least one group deep inside the walled run for that edge to be hidden too.
 * Writes the worst-case wall top/setback across the whole tested range. */
static int tg_r13_band_covers(const TG_NodeList *nl, int g0, int g1,
                              int is_left, double *out_top, double *out_lat)
{
    const double side = is_left ? 1.0 : -1.0;
    /* TD5_TG_FAR_GROUP is TD5_TG_SPANS_PER_ENTRY, spelled that way here because
     * the far-band block that names it sits further down the file. */
    int s, lo = g0 - TD5_TG_SPANS_PER_ENTRY, hi = g1 + TD5_TG_SPANS_PER_ENTRY;
    double top = 1e30, lat = 0.0;

    /* [BAND CULL 2026-09-07] The window is the band's own extent [g0,g1] PLUS a
     * one-entry margin either side (the margin exists because a band can be seen
     * obliquely, from past the end of the tree wall that hides it head-on).
     *
     * This used to bail on the FIRST span anywhere in that window without a tree
     * band, margin included. A tree line with a single gap four spans beyond the
     * band therefore disabled the cull completely and the band was drawn in full,
     * behind the trees -- which is the reported "this geometry is under existing
     * trees".
     *
     * Split the requirement instead of loosening it:
     *   - over the band's OWN spans the wall must still be unbroken (hard: those
     *     are the spans the band is actually seen at, head-on);
     *   - in the MARGIN a small number of gaps is tolerated, because one missing
     *     tree beyond the band's end does not expose the band behind it.
     * The measured top/lat still come only from spans that HAVE a wall, so a
     * tolerated gap can never loosen the height or lateral test itself.
     * TD5RE_R13_BAND_MARGIN=0 restores the old all-or-nothing window. */
    int misses = 0;
    const int margin_slack =
        td5_env_flag_on("TD5RE_R13_BAND_MARGIN") ? 1 : 0;

    if (lo < 0) lo = 0;
    if (hi > nl->count - 2) hi = nl->count - 2;
    for (s = lo; s <= hi; s++) {
        double t, l;
        if (!tg_r13_band_side(nl, s, side, &t, &l)) {
            if (s >= g0 && s <= g1) return 0;      /* band's own extent: hard */
            if (++misses > margin_slack) return 0; /* margin: bounded slack */
            continue;
        }
        if (t < top) top = t;
        if (l > lat) lat = l;
    }
    if (top > 1e29) return 0;         /* nothing measured -- do not cull blind */
    *out_top = top;
    *out_lat = lat;
    return 1;
}

static long   s_r14_wraps;

static double s_r14_wrap_reach;

/* One wrap wall: from the band's end at `node` on `side`, outward to the shore.
 * `surf` is the water surface the wall is wrapping around -- a river's under a
 * bridge run, a sea's at a water biome. The emitter does not care which: it
 * walks outward along the ground and stops where the ground reaches `surf`. */
static int tg_r14_emit_wrap(const TG_FBHook *h, int node, double surf,
                            double side)
{
    const TG_NodeList *nl = h->nl;
    const TG_Node *n = &nl->v[node];
    double px[4 * TD5_TG_R14_WRAP_COLS * 2], py[4 * TD5_TG_R14_WRAP_COLS * 2];
    double pz[4 * TD5_TG_R14_WRAP_COLS * 2], uu[4 * TD5_TG_R14_WRAP_COLS * 2];
    double vv[4 * TD5_TG_R14_WRAP_COLS * 2];
    int seg_page[TD5_TG_R14_WRAP_COLS * 2], seg_nq[TD5_TG_R14_WRAP_COLS * 2];
    TG_TopoChain tc;
    size_t m0;
    double bh = 0.0, bk = 0.0, e0, reach, d0drop;
    const double lx = n->tz * side, lz = -n->tx * side;
    const double vt = TD5_TG_FACADE_UV_INSET, vb = 1.0 - TD5_TG_FACADE_UV_INSET;
    int nseg = 0, nv = 0, c;

    tg_r12_band_params(node, &bh, &bk);
    if (!(bh > 0.0)) return 1;                    /* no wall ends here */
    if (*h->nmesh + 1 >= h->maxmesh) return 1;

    e0 = n->width * 0.5 + tg_flora_gap_clear(nl, node, side, bk);
    tg_topo_chain(nl, node, side > 0.0 ? 1 : 0, &tc);
    d0drop = tg_topo_drop_at(&tc, e0);

    /* Where the ground meets the water: march out and stop at the first sample
     * at or below the surface. No crossing inside the reach = run the full
     * reach and taper anyway, so the wrap never ends on a face either. */
    reach = e0 + TD5_TG_R14_WRAP_LEN;
    for (c = 1; c <= TD5_TG_R14_WRAP_COLS * 4; c++) {
        const double t = e0 + TD5_TG_R14_WRAP_LEN
                            * (double)c / (double)(TD5_TG_R14_WRAP_COLS * 4);
        if (n->y - tg_topo_drop_at(&tc, t) <= surf) { reach = t; break; }
    }
    if (reach <= e0 + 2000.0) return 1;           /* no shore to wrap around */

    for (c = 0; c < TD5_TG_R14_WRAP_COLS; c++) {
        const double f0 = (double)c / (double)TD5_TG_R14_WRAP_COLS;
        const double f1 = (double)(c + 1) / (double)TD5_TG_R14_WRAP_COLS;
        const double t0 = e0 + (reach - e0) * f0;
        const double t1 = e0 + (reach - e0) * f1;
        /* Base on the terrain, height tapering linearly to 0 at the shore. */
        const double y0 = n->y - TD5_TG_TREELINE_SINK
                        - (tg_topo_drop_at(&tc, t0) - d0drop);
        const double y1 = n->y - TD5_TG_TREELINE_SINK
                        - (tg_topo_drop_at(&tc, t1) - d0drop);
        const double h0 = bh * (1.0 - f0), h1 = bh * (1.0 - f1);
        const double x0 = n->x + lx * t0, z0 = n->z + lz * t0;
        const double x1 = n->x + lx * t1, z1 = n->z + lz * t1;
        /* Square tiles, the rule the band and the forest-crossing fold share. */
        const double u0 = (t0 - e0) / bh, u1 = (t1 - e0) / bh;
        int f;
        for (f = 0; f < 2; f++) {       /* both windings: seen in and seen out */
            if (nv + 4 > 4 * TD5_TG_R14_WRAP_COLS * 2) break;
            if (!f) {
                px[nv]=x0; py[nv]=y0;    pz[nv]=z0; uu[nv]=u0; vv[nv]=vb; nv++;
                px[nv]=x1; py[nv]=y1;    pz[nv]=z1; uu[nv]=u1; vv[nv]=vb; nv++;
                px[nv]=x1; py[nv]=y1+h1; pz[nv]=z1; uu[nv]=u1; vv[nv]=vt; nv++;
                px[nv]=x0; py[nv]=y0+h0; pz[nv]=z0; uu[nv]=u0; vv[nv]=vt; nv++;
            } else {
                px[nv]=x0; py[nv]=y0+h0; pz[nv]=z0; uu[nv]=u0; vv[nv]=vt; nv++;
                px[nv]=x1; py[nv]=y1+h1; pz[nv]=z1; uu[nv]=u1; vv[nv]=vt; nv++;
                px[nv]=x1; py[nv]=y1;    pz[nv]=z1; uu[nv]=u1; vv[nv]=vb; nv++;
                px[nv]=x0; py[nv]=y0;    pz[nv]=z0; uu[nv]=u0; vv[nv]=vb; nv++;
            }
            seg_page[nseg] = TD5_TG_PAGE_TREELINE;
            seg_nq[nseg] = 1;
            nseg++;
        }
    }
    if (nv <= 0) return 1;
    s_r14_wraps++;
    s_r14_wrap_reach += reach - e0;
    tg_acct_n(TG_ACCT_R14_COAST, node, nseg);
    tg_acct_n(TG_ACCT_TREE, node, nseg);
    m0 = h->blk->len;
    h->moff[(*h->nmesh)++] = m0;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, nv,
                            seg_page, seg_nq, nseg)) return 0;
    /* Marked COAST, and the exemption is EARNED rather than smuggled: the wrap
     * stands on the boundary node, which is the river rectangle's own near
     * edge, so every column is inside that rectangle and the R9 over-water
     * audit would drop the whole mesh as massing on water. It is not massing --
     * it is shore geometry that descends the bank and reaches ZERO HEIGHT
     * exactly at the waterline by construction, which is the same claim
     * TG_GK_COAST already licenses for the shore band beside it. The mark is
     * span-scoped to `node`, so it licenses nothing anywhere else. */
    tg_guard_mark(m0, h->blk->len, TG_GK_COAST, node);
    return 1;
}

/* Fire the wrap on a LAND span whose neighbour carries WATER, at the node the
 * band's own last (or first) quad ends on. Asked for every span, before the
 * band's early-outs, because the wrap belongs to the span BESIDE the water and
 * the water's own spans return early.
 *
 * TWO KINDS OF WATER END A TREE LINE, and the user's item names both: the
 * RIVER under a bridge run (the reported span 1351) and the SEA of a water
 * biome (a COAST run, which carries no tree line of its own so every band
 * meeting it stops). They differ only in where the surface is and in whether
 * there is a shore on one side or both -- a river gorge has banks on both, a
 * coast has sea on one side and dry land on the other, and wrapping the dry
 * side around nothing would be a wall turning a corner for no reason. */
static int tg_r14_wrap_here(const TG_FBHook *h)
{
    const int si = h->si;
    const TG_NodeList *nl = h->nl;
    double t, l, ws;
    int s;

    if (!td5_env_flag_on("TD5RE_R14_COAST_WRAP")) return 1;   /* default ON */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TREELINE")) return 1;
    if (!td5_env_flag_on("TD5RE_R12_FLORA_BAND")) return 1;
    if (si <= tg_r13_band_first_span() || si + 1 >= nl->count) return 1;
    if (tg_span_in_bridge_run(si)) return 1;                  /* not a land span */

    /* RIVER. Entry mouth: the band's far end sits on node si+1. Exit mouth: its
     * near end sits on node si. Both banks, so both sides. */
    if (si + 1 < nl->count && tg_span_in_bridge_run(si + 1)
        && tg_water_span_clear(si + 1)) {
        ws = tg_bridge_water_surf_y(nl, si + 1);
        if (!tg_r14_emit_wrap(h, si + 1, ws,  1.0)) return 0;
        if (!tg_r14_emit_wrap(h, si + 1, ws, -1.0)) return 0;
    }
    if (si - 1 >= 0 && tg_span_in_bridge_run(si - 1)
        && tg_water_span_clear(si - 1)) {
        ws = tg_bridge_water_surf_y(nl, si - 1);
        if (!tg_r14_emit_wrap(h, si, ws,  1.0)) return 0;
        if (!tg_r14_emit_wrap(h, si, ws, -1.0)) return 0;
    }

    /* SEA. Fire where the wall this span carries is the LAST one before a water
     * biome (or the first one after it), and only on the side the sea is on.
     * Presence is read through tg_r13_band_side, the same predicate R13 BAND's
     * cull uses, so "a wall ends here" means the same thing to both. */
    for (s = si - 1; s <= si + 1; s += 2) {
        double sd;
        if (s < 0 || s + 1 >= nl->count) continue;
        if (tg_span_in_bridge_run(s)) continue;        /* the river case above */
        if (!tg_biome_span_has_water(s)) continue;
        sd = tg_water_side(s);
        if (sd == 0.0) continue;
        if (!tg_r13_band_side(nl, si, sd, &t, &l)) continue;  /* no wall to turn */
        if (tg_r13_band_side(nl, s, sd, &t, &l)) continue;    /* not an end     */
        if (!tg_r14_emit_wrap(h, (s > si) ? si + 1 : si,
                              tg_sea_level_y(nl, s), sd)) return 0;
    }
    return 1;
}

int tg_emit_fb_flora(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    /* [R12 item 9] band height and setback at the quad's TWO nodes. */
    double bh[2] = { 0.0, 0.0 }, bk[2] = { 0.0, 0.0 };
    double band;
    int s, sj;
    /* [R12 CROSS item 5] Where a forest side road passes, THIS side's band is
     * cut; tg_r12_fcross_emit_fold lays the folded return walls that close the
     * opening, so the cut is never a hole. */
    double fx_side = 0.0, fx_reach = 0.0;
    const int fxc = tg_r12_fcross_at(nl, si, &fx_side, &fx_reach);

    /* [R14 COAST item 5b] Asked FIRST, because the wrap closes the cut a bridge
     * run makes and the run's own spans return early below. */
    if (!tg_r14_wrap_here(h)) return 0;

    /* Default ON (2026-08-26); TD5RE_AUTOTRACK_TREELINE=0 disables the band. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TREELINE"))
        { tg_r12_band_note(si, TG_R12_BAND_OFF); return 1; }
    /* [R13 BAND item 1a] The wall stands 11000 out; the grid needs no clearance
     * from it. Only span 0 stays bare -- see tg_r13_band_grid. */
    if (si <= tg_r13_band_first_span())
        { tg_r12_band_note(si, TG_R12_BAND_GRID); return 1; }
    if (si + 1 >= nl->count)
        { tg_r12_band_note(si, TG_R12_BAND_END); return 1; }
    if (tg_span_in_bridge_run(si))               /* see the river from the deck */
        { tg_r12_band_note(si, TG_R12_BAND_BRIDGE); return 1; }
    /* [R12 item 9] Per-NODE height/setback off the HARD cell; see
     * tg_r12_band_params. Knob off = the dithered per-span pair, unchanged. */
    if (td5_env_flag_on("TD5RE_R12_FLORA_BAND")) {
        tg_r12_band_params(si,     &bh[0], &bk[0]);
        tg_r12_band_params(si + 1, &bh[1], &bk[1]);
        /* u tiling comes from the RUN's own height, never the ramped one: du has
         * to match on both sides of every shared edge or the wall shows a u
         * seam where the ramp changes it. */
        band = tg_treeline_height(&k_biomes[tg_biome_cell_index(si)]);
    } else {
        bh[0] = bh[1] = band = tg_treeline_height(h->b);
        bk[0] = bk[1] = tg_treeline_back(h->b);
    }
    if (!(bh[0] > 0.0) && !(bh[1] > 0.0))
        { tg_r12_band_note(si, TG_R12_BAND_BIOME); return 1; }
    if (!(band > 0.0)) band = (bh[0] > bh[1]) ? bh[0] : bh[1];
    /* the far node's own span index, clamped so the clearance query stays in
     * range on the last quad of the list. */
    sj = (si + 2 < nl->count) ? si + 1 : si;
    tg_r12_band_note(si, TG_R12_BAND_OK);

    /* ONE MESH PER SIDE, not one for both: a mesh spanning both verges has a
     * bounding sphere wider than the band is far away, so the culler could
     * never reject it. Two tight spheres cull properly. */
    for (s = 0; s < 2; s++) {
        double px[4 * (TD5_TG_TL_MAX_BODY + 1)], py[4 * (TD5_TG_TL_MAX_BODY + 1)];
        double pz[4 * (TD5_TG_TL_MAX_BODY + 1)], uu[4 * (TD5_TG_TL_MAX_BODY + 1)];
        double vv[4 * (TD5_TG_TL_MAX_BODY + 1)];
        const double side = s ? 1.0 : -1.0;
        const TG_Node *n0 = &nl->v[si];
        const TG_Node *n1 = &nl->v[si + 1];
        const double lx0 = n0->tz * side, lz0 = -n0->tx * side;
        const double lx1 = n1->tz * side, lz1 = -n1->tx * side;
        /* Clear of any branch carriageway, same rule as the trees. [R12 item 9]
         * PER NODE: the far end takes span si+1's own setback, which is the
         * value the NEXT quad computes for its near end, so the two abut
         * exactly even where the setback or the fork clearance changes. */
        const double d  = tg_flora_gap_clear(nl, si, side, bk[0]);
        const double d1 = tg_flora_gap_clear(nl, sj, side, bk[1]);
        const double e0 = n0->width * 0.5 + d, e1 = n1->width * 0.5 + d1;
        const double bx = n0->x + lx0 * e0, bz = n0->z + lz0 * e0;
        const double fx = n1->x + lx1 * e1, fz = n1->z + lz1 * e1;
        const double by = n0->y - TD5_TG_TREELINE_SINK;
        const double fy = n1->y - TD5_TG_TREELINE_SINK;
        /* [R11 item 1] Square tiles, cumulative u; see tg_r11_treeline_fit.
         * flag off = the shipped alternating 0..1 tile (a 1:8 stretch). */
        const int    fit = tg_r11_treeline_fit();
        const double du  = (double)TD5_TG_SPAN_LENGTH / band;
        const double ub  = floor((double)si * du);      /* whole tiles, dropped */
        const double u0  = fit ? ((double)si * du - ub)
                               : (((si & 1) == 0) ? 0.0 : 1.0);
        const double u1  = fit ? ((double)(si + 1) * du - ub) : (1.0 - u0);
        /* [R11 item 1] Half-texel V inset, the R4 item 5 cure for the "black
         * line at the top" the wrapped sampler paints at v = 0. */
        const double vt  = fit ? TD5_TG_FACADE_UV_INSET       : 0.0;
        const double vb  = fit ? 1.0 - TD5_TG_FACADE_UV_INSET : 1.0;
        int seg_page = TD5_TG_PAGE_TREELINE, seg_nq = 1;
        int n = 0;

        if (fxc && side == fx_side) continue;     /* [R12 CROSS item 5] the CUT */
        if (*h->nmesh + 1 >= h->maxmesh)             /* out of slots this entry */
            { tg_r12_band_note(si, TG_R12_BAND_SLOTS); return 1; }
        if (fit && tg_r12_treeline_density()) {
            /* [R12 TEX item 1 + R12 FLORA item 9, merged] Crown + K mirrored
             * body strips, expressed as FRACTIONS of the wall rather than as
             * absolute heights, so FLORA's per-node heights bh[0] (near) and
             * bh[1] (far) still apply. That matters at a biome edge, where the
             * band tapers to zero over the last spans and the two ends of one
             * quad are deliberately different heights: a strip stack built on a
             * single `band` would step instead of taper, and neighbouring spans
             * would stop sharing their endpoint heights.
             *
             * kb and the tile size come from `band`, the run's representative
             * height, so TEXEL DENSITY is set by the wall itself and does not
             * collapse on a tapering end. */
            const double du12 = (double)TD5_TG_SPAN_LENGTH / TD5_TG_TL_TILE_U;
            const double ua   = tg_r12_tl_fold((double)si       * du12);
            const double ub2  = tg_r12_tl_fold((double)(si + 1) * du12);
            const double bodyf = 1.0 - TD5_TG_TL_CROWN_V;
            /* [R16 item C] VERTICAL tile size only -- du12 above keeps TILE_U so
             * the horizontal square-tile aspect and the u wrap are unchanged. A
             * taller vertical tile means fewer mirrored body repeats up the wall
             * (the reported vertical repetition). Gated OFF (factor 1.0). */
            const double tile_v = TD5_TG_TL_TILE_U *
                (td5_env_flag_off("TD5RE_R16_TREELINE_VSCALE")
                     ? TD5_TG_R16_TREELINE_VSCALE : 1.0);
            int kb = (int)floor((band / tile_v - TD5_TG_TL_CROWN_V)
                                / bodyf + 0.5);
            double total, fc;
            int j;
            if (kb < 1) kb = 1;
            if (kb > TD5_TG_TL_MAX_BODY) kb = TD5_TG_TL_MAX_BODY;
            total = TD5_TG_TL_CROWN_V + (double)kb * bodyf;
            /* Body strips from the BASE up, alternating v direction so each
             * join shares a row with its neighbour. Strip j spans fractions
             * [j*bodyf, (j+1)*bodyf] of `total`; strip 0 sits on the ground. */
            for (j = 0; j < kb; j++) {
                /* Strip kb-1 is the one that meets the crown, and its TOP must
                 * read v = CROWN_V. Parity from the distance to that strip. */
                const int up = ((kb - 1 - j) & 1) == 0;
                const double vlo = up ? 1.0 - TD5_TG_FACADE_UV_INSET
                                      : TD5_TG_TL_CROWN_V;      /* at strip base */
                const double vhi = up ? TD5_TG_TL_CROWN_V
                                      : 1.0 - TD5_TG_FACADE_UV_INSET; /* at top  */
                const double f0 = (double)j * bodyf / total;
                const double f1 = (double)(j + 1) * bodyf / total;
                px[n]=bx; py[n]=by+bh[0]*f0; pz[n]=bz; uu[n]=ua;  vv[n]=vlo; n++;
                px[n]=fx; py[n]=fy+bh[1]*f0; pz[n]=fz; uu[n]=ub2; vv[n]=vlo; n++;
                px[n]=fx; py[n]=fy+bh[1]*f1; pz[n]=fz; uu[n]=ub2; vv[n]=vhi; n++;
                px[n]=bx; py[n]=by+bh[0]*f1; pz[n]=bz; uu[n]=ua;  vv[n]=vhi; n++;
            }
            /* CROWN on top: page rows 0..23, keyed silhouette against the sky.
             * Its base v must equal the top v of the last body strip, which the
             * parity above forces to CROWN_V; its top is the wall's own height
             * at each end, so the crown line tapers with the band. */
            fc = (double)kb * bodyf / total;
            px[n]=bx; py[n]=by+bh[0]*fc; pz[n]=bz; uu[n]=ua;  vv[n]=TD5_TG_TL_CROWN_V; n++;
            px[n]=fx; py[n]=fy+bh[1]*fc; pz[n]=fz; uu[n]=ub2; vv[n]=TD5_TG_TL_CROWN_V; n++;
            px[n]=fx; py[n]=fy+bh[1];    pz[n]=fz; uu[n]=ub2; vv[n]=vt; n++;
            px[n]=bx; py[n]=by+bh[0];    pz[n]=bz; uu[n]=ua;  vv[n]=vt; n++;
            seg_nq = n / 4;
        } else {
            /* quad loop: near-bottom, far-bottom, far-top, near-top; v=1 at the
             * base, matching the page convention that row 0 is the TOP. */
            px[0] = bx; py[0] = by;         pz[0] = bz; uu[0] = u0; vv[0] = vb;
            px[1] = fx; py[1] = fy;         pz[1] = fz; uu[1] = u1; vv[1] = vb;
            px[2] = fx; py[2] = fy + bh[1]; pz[2] = fz; uu[2] = u1; vv[2] = vt;
            px[3] = bx; py[3] = by + bh[0]; pz[3] = bz; uu[3] = u0; vv[3] = vt;
            n = 4;
        }

        h->moff[*h->nmesh] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                &seg_page, &seg_nq, 1)) return 0;
        (*h->nmesh)++;
        tg_acct(TG_ACCT_TREE, si);   /* tree-line band, one per side */
    }
    return 1;
}

/* [R5 item 18] Tall park trees. The user asked a curvy park to justify itself
 * with "a lot of trees ... trees that take up to the ceiling like the sections
 * at the end of Moscow or the middle of Australia", and to "expand the library
 * of available resources". The 10-page borrowed tree set took nothing from
 * Moscow (level023), whose texture set is unusually rich in foliage; this plants
 * a TALL Moscow canopy billboard set well BACK behind each tree biome's own
 * verge planting, so the park (and every billboard-tree biome) gains a high
 * canopy backdrop rising above the near trees rather than a flat hedge line.
 * Real-texture only -- the pages are Moscow art -- and gated for an A/B via
 * TD5RE_R5_FLORA_PARKTREES (default ON). One billboard per qualifying span,
 * accounted under the FLORA area. */
int tg_emit_fb_park_trees(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    /* [R17 TERRAIN item 4] "the whole transition between these two biomes has no
     * geometry -- remove the logic of transitioning with removals on every other
     * span." This supplementary park-tree layer gated on the DITHERED per-span
     * biome (h->b, from tg_biome_for_span), so on a paved<->tree-biome blend band
     * it emitted nothing on every span the dither resolved to the facade side.
     * Where the tree biome ALSO carries no hard-keyed tree-line band (COAST and
     * ORIENTAL have tg_treeline_height==0), that left only the flat ground skirt
     * across the transition -- geometry present, then gone, every other span.
     * Take the biome from tg_scenery_biome_index instead, the same R11 hard-edge
     * source the NEAR verge trees already use: it hard-snaps at a paved/unpaved
     * boundary (so the trees run continuously to the hard edge and stop cleanly)
     * while keeping the categorical dither for billboard<->billboard edges, where
     * the tree-species interleave is the good kind of blend. Default ON;
     * TD5RE_R17_FLORA_HARDEDGE=0 restores the dithered per-span biome. */
    const TG_Biome *b = td5_env_flag_on("TD5RE_R17_FLORA_HARDEDGE")
                      ? &k_biomes[tg_scenery_biome_index(si)] : h->b;
    unsigned int hh;
    double side, gap, tw, th, jit, cx, cz;
    int v, page;

    if (!td5_env_flag_on("TD5RE_R5_FLORA_PARKTREES")) return 1;
    if (!tg_real_textures_enabled())  return 1;   /* pages are Moscow art */
    if (k_r5_flora_tree_count <= 0)   return 1;
    if (!b->billboard || b->tree_n <= 0) return 1;/* tree biomes only, not facades */
    if (si <= TD5_TG_GRID_SPAN)       return 1;   /* keep the grid area clear */
    if (si + 1 >= nl->count)          return 1;
    if (tg_span_in_bridge_run(si))    return 1;   /* deck is clear */
    if (*h->nmesh + 1 >= h->maxmesh)  return 1;

    /* Sparser than the near trees: a grove on roughly one span in three, so the
     * tall canopy reads as clumps rather than a continuous second wall. */
    hh = (unsigned)si * 2246822519u + 0x2545F491u;
    if ((int)(hh >> 29) > (b->density >> 1)) return 1;

    side = ((hh >> 4) & 1) ? 1.0 : -1.0;
    if (tg_side_blocked(si, side)) return 1;
    {   /* [R12 CROSS item 5] never in a forest side road's lane, same reason as
         * the verge tree above (these plant at 2600..5800 lateral). */
        double fs = 0.0, fr = 0.0;
        if (tg_r12_fcross_at(nl, si, &fs, &fr) && fs == side) return 1;
    }

    /* [R6 items 7/18] Default ON; TD5RE_R6_FLORA=0 restores the R5 park tree
     * (grove-wall page in the upright rotation + 4200-wide billboards) for A/B. */
    if (td5_env_flag_on("TD5RE_R6_FLORA")) {
        v = tg_flora_upright_index(hh >> 13);    /* skip the grove-backdrop wall */
        if (v < 0) return 1;
    } else {
        v = (int)((hh >> 13) % (unsigned)k_r5_flora_tree_count);
    }
    page = tg_flora_tree_slot(v);

    /* Tall: the near verge trees top out around 7200 raw (the palm); these reach
     * ~10000 raw (~40 world units) so they stand above that as a canopy. */
    jit = 1.40 + (double)((hh >> 9) % 61) * 0.01;   /* 1.40 .. 2.00 */
    /* WIDTH [R6 items 7/18]: was 4200*jit (up to 8400). A single flat billboard
     * that wide reads as a WALL/treeline beside the road and looms over a curved
     * approach ("bigger than the area ... spill onto the road"). A tree is
     * slender -- the near-verge species measure ~2900..4700 wide and read
     * correctly -- so narrow to 2600*jit (3640..5200) while KEEPING the extra
     * height the "ceiling-tall Moscow trees" request wanted. */
    tw  = (td5_env_flag_on("TD5RE_R6_FLORA") ? 2600.0 : 4200.0) * jit;
    th  = 5200.0 * jit;
    gap = 2600.0 + (double)((hh >> 5) % 3200);      /* set BACK, behind the verge */
    gap = tg_flora_gap_clear(nl, si, side, gap);    /* never on a branch */

    {   /* [R7 item 18] on the ground, never on water/coastline */
        double base_y;
        if (!tg_flora_plant(nl, si, b, side, gap, tw, &cx, &cz, &base_y))
            return 1;

        /* [R12 item 4] shared spacing rule -- see tg_r12_flora_accept. */
        if (!tg_r12_flora_accept(si, "park", page, side, cx, cz, tw, th))
            return 1;
        tg_flora_diag(nl, si, "park", page, side, tw, th, cx, cz);
        h->moff[*h->nmesh] = h->blk->len;
        if (!tg_emit_billboard_mesh(h->blk, cx, base_y, cz, tw * 0.5, th, page, 1))
            return 0;
        (*h->nmesh)++;
        tg_acct(TG_ACCT_R5_FLORA, si);
        return 1;
    }
}

/* Drop below road level of the terrain at distance `d` out from the road edge,
 * interpolated along the side's cross-section. Same walk tg_flora_plant does
 * for a tree trunk; kept local so the flora rule stays one owner's code. */
double tg_infra_ground_dy(const TG_NodeList *nl, int si, double side,
                                 double d, double water_side)
{
    TG_GroundProf p;
    int k;

    tg_ground_side(nl, si, side > 0.0, water_side, &p);
    if (p.n <= 0)              return 0.0;
    if (d <= p.d[0])           return p.dy[0];
    if (d >= p.d[p.n - 1])     return p.dy[p.n - 1];
    for (k = 0; k + 1 < p.n; k++)
        if (d <= p.d[k + 1]) {
            const double t = (d - p.d[k]) / (p.d[k + 1] - p.d[k]);
            return p.dy[k] + t * (p.dy[k + 1] - p.dy[k]);
        }
    return p.dy[p.n - 1];
}

/* Do the green biomes want a pond here? Ponds come in ones, not runs -- a
 * puddle chain along a verge would read as a leak, not as landscape. */
static int tg_pond_here(const TG_Biome *b, int si)
{
    if (b->water)               return 0;   /* already beside sea or river */
    if (tg_city_sidewalk_w(b) > 0.0) return 0;  /* not on a paved street   */
    if (strcmp(b->name, "FIELDS") && strcmp(b->name, "FOREST")
        && strcmp(b->name, "ALPINE")) return 0;
    return (((unsigned)si * 0x1B873593u) >> 26) == 0u;   /* ~1 span in 64 */
}

static int tg_emit_pond(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1 = &nl->v[si + 1];
    const unsigned int hh = (unsigned)si * 0x1B873593u;
    const double side = (hh & 0x100u) ? 1.0 : -1.0;
    const double lx0 = n0->tz * side, lz0 = -n0->tx * side;
    const double lx1 = n1->tz * side, lz1 = -n1->tx * side;
    double px[4], py[4], pz[4], uu[4], vv[4];
    double dy[4], lo, hi, near_d, far_d, e0, e1, y;
    int i, seg_page = TD5_TG_PAGE_WATER, seg_nq = 1;

    /* [POND 2026-09-06] Default OFF (was flag_on, i.e. default ON).
     * tg_pond_here deliberately fires on DRY biomes (FIELDS/FOREST/ALPINE) and
     * the emitter lays a flat 4-vert WATER quad with no basin and no bank --
     * there is no terrain-carving pass, so a sunk pond would be invisible. The
     * result reads exactly as reported: a water texture sitting on grass.
     * Flipped to flag_off rather than deleted so the feature comes back with
     * TD5RE_R9_INFRA_PONDS=1 once it has a rim/bank to sit in.
     * [user report 2026-09-06: "there's water geometry over the grass"] */
    if (!td5_env_flag_off("TD5RE_R9_INFRA_PONDS")) return 1;
    if (!tg_pond_here(h->b, si))    return 1;
    if (tg_side_blocked(si, side))  return 1;
    if (*h->nmesh + 1 >= h->maxmesh) return 1;

    /* Push the near edge out past any branch corridor bowing into this
     * lateral, and carry the same push to the far edge so the sheet keeps its
     * size instead of being squeezed to a stripe. */
    near_d = tg_carriageway_clear_gap(nl, si, side, TD5_TG_POND_NEAR,
                                      TD5_TG_CARRIAGEWAY_MARGIN);
    far_d  = near_d + (TD5_TG_POND_FAR - TD5_TG_POND_NEAR);

    dy[0] = tg_infra_ground_dy(nl, si,     side, near_d, 0.0);
    dy[1] = tg_infra_ground_dy(nl, si + 1, side, near_d, 0.0);
    dy[2] = tg_infra_ground_dy(nl, si + 1, side, far_d,  0.0);
    dy[3] = tg_infra_ground_dy(nl, si,     side, far_d,  0.0);
    lo = hi = dy[0];
    for (i = 1; i < 4; i++) {
        if (dy[i] < lo) lo = dy[i];
        if (dy[i] > hi) hi = dy[i];
    }
    /* FLATNESS GATE. Both the cross-section AND the along-road grade have to be
     * flat: the node Y difference is the second half of that test, and without
     * it a pond on a hill would pass on its cross-section alone. */
    if (hi - lo > TD5_TG_POND_FLAT) return 1;
    if (fabs(n1->y - n0->y) > TD5_TG_POND_FLAT) return 1;

    e0 = n0->width * 0.5;
    e1 = n1->width * 0.5;
    /* Lowest corner wins, so the sheet never floats above the ground it lies
     * on -- it can only ever be buried by at most the flatness tolerance. */
    y = (n0->y < n1->y ? n0->y : n1->y) - hi + TD5_TG_POND_LIFT;

    px[0] = n0->x + lx0 * (e0 + near_d); pz[0] = n0->z + lz0 * (e0 + near_d);
    px[1] = n1->x + lx1 * (e1 + near_d); pz[1] = n1->z + lz1 * (e1 + near_d);
    px[2] = n1->x + lx1 * (e1 + far_d);  pz[2] = n1->z + lz1 * (e1 + far_d);
    px[3] = n0->x + lx0 * (e0 + far_d);  pz[3] = n0->z + lz0 * (e0 + far_d);
    for (i = 0; i < 4; i++) {
        py[i] = y;
        uu[i] = px[i] / TD5_TG_WATER_TILE;   /* same UV rule as the sea plane */
        vv[i] = pz[i] / TD5_TG_WATER_TILE;
    }

    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4,
                            &seg_page, &seg_nq, 1)) return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_WATER, si);   /* a pond IS water, not a new element kind */
    s_r9_infra_ponds++;
    if (td5_env_flag_off("TD5RE_R9_INFRA_REPORT"))
        TD5_LOG_I(LOG_TAG, "trackgen: [R9 INFRA] span %4d %-5s pond "
                  "biome=%s ground-spread=%.0f", si,
                  side > 0.0 ? "left" : "right", h->b->name, hi - lo);
    return 1;
}

/* Street furniture for span si. Density is a per-(span, side) hash so the two
 * kerbs are independent and a re-run of the same seed lays the same street. */
int tg_emit_fb_infra(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    const TG_Biome *sb = &k_biomes[tg_scenery_biome_index(si)];
    const double sw = tg_city_sidewalk_w(sb);
    const int paved = (sw > 0.0);
    const TG_Node *n = &nl->v[si];
    int menu[8], nmenu, s;

    if (si <= TD5_TG_GRID_SPAN)  return 1;      /* keep the start grid clear */
    if (si + 1 >= nl->count)     return 1;
    if (tg_span_in_bridge_run(si)) return 1;    /* the deck carries rails only */

    /* Ponds share this hook (and this area's one guard mark) rather than
     * taking a dispatcher slot of their own -- both are INFRA scenery on the
     * verge, and the guard has to judge them by the same rule either way.
     * They keep their OWN knob, so either half can be A/B'd alone. */
    if (!tg_emit_pond(h)) return 0;

    if (!td5_env_flag_on("TD5RE_R9_INFRA_PROPS")) return 1;
    nmenu = tg_infra_menu(sb, paved, menu);
    if (nmenu <= 0) return 1;

    for (s = 0; s < 2; s++) {
        const double side = s ? 1.0 : -1.0;
        /* Independent hash per side: 0x9E3779B9 is already the prop layer's
         * multiplier, so mix in the side and a different constant or the
         * furniture would land on exactly the spans the spectators use. */
        const unsigned int hh = ((unsigned)si * 0x27220A95u)
                              ^ ((unsigned)(s + 1) * 0x85EBCA6Bu);
        double gap, base_y;
        int kind;

        /* ~1 span in 5 per side. Denser than the statues (1 in 29) because
         * furniture is what makes a street look inhabited, sparse enough that
         * a straight does not read as a warehouse aisle. */
        if ((hh >> 28) > 2u) continue;

        kind = menu[(hh >> 5) % (unsigned)nmenu];
        /* Roadworks replace the menu pick occasionally, on ONE side only, so
         * they read as a works site rather than a decoration. */
        kind = tg_infra_sign_filter(sb, si, side, paved, kind, hh);
        kind = tg_infra_awning_filter(si, side, paved, kind, hh);
        if (((hh >> 13) & 0x3Fu) == 0u) kind = IP_WORKY;
        else if (((hh >> 19) & 0x3Fu) == 0u) kind = IP_REDTAPE;

        if (kind == IP_RICKSHAW && strcmp(sb->name, "ORIENTAL")) continue;

        if (td5_env_flag_off("TD5RE_R9_INFRA_REPORT")
            && s_r9_infra_reported < TD5_TG_INFRA_REPORT_MAX) {
            s_r9_infra_reported++;
            TD5_LOG_I(LOG_TAG,
                      "trackgen: [R9 INFRA] span %4d %-5s %-14s biome=%s %s",
                      si, side > 0.0 ? "left" : "right", k_infra_names[kind],
                      sb->name, paved ? "pavement" : "verge");
        }

        if (paved) {
            /* Across the pavement, between 0.30 and 0.70 of its width -- clear
             * of the kerb face on one side and of the frontage on the other. */
            gap = sw * (0.30 + 0.40 * (double)((hh >> 8) & 0xFFu) / 255.0);
            base_y = n->y + tg_city_kerb_h(sb);
            /* [R13 PROPS item 5a] ANCHOR. Everything else on this menu stands
             * on the pavement, so the shared setback is right for it. An awning
             * hangs on the wall, so it takes the FRONTAGE setback instead: the
             * pavement WIDTH at this span (the same tg_city_sidewalk_w_at the
             * facade itself is set back by) less its own half depth, which puts
             * its back edge on the facade line rather than in mid-air. */
            if (kind == IP_CANOPY && td5_env_flag_on("TD5RE_R13_AWNING"))
                gap = tg_city_sidewalk_w_at(nl, si, sb)
                    - k_infra_props[IP_CANOPY].w * 0.5;
        } else {
            /* On a verge there is no kerb to stand on, and the skirt DROPS
             * away from the road -- so road height is the wrong footing and a
             * bench two lanes out would hang in the air over falling ground.
             * Rest on the sampled terrain instead, the same rule R7 FLORA gave
             * tree trunks. */
            gap = TD5_TG_VERGE_W + 200.0
                + (double)((hh >> 8) & 0x1FFu);
            base_y = n->y - tg_infra_ground_dy(nl, si, side, gap, 0.0);
        }
        if (!tg_infra_place(h, kind, side, gap, base_y)) return 0;
    }
    return 1;
}

/* Spans from si to the nearest END of its tunnel run, capped. tg_span_in_tunnel
 * is a pure hash of si, so this is a bounded walk of at most cap+1 steps, not a
 * search over the track. Negative si is safe: the function early-outs on
 * si <= TD5_TG_GRID_SPAN + 40 before touching anything. */
static int tg_tunnel_edge_dist(int si, int cap)
{
    int d;
    for (d = 0; d <= cap; d++)
        if (!tg_span_in_tunnel(si - d) || !tg_span_in_tunnel(si + d))
            return d;
    return cap + 1;
}

/* [R11 TEX item 2] "at the tunnel entrance near span 294 there is a gray-ish
 * stripe texture beside the tunnel -- remove that page from the pool it is
 * being drawn from."
 *
 * It is not a pool: the piece beside the mouth is the SHOULDER box below, and it
 * has one hard-coded page, TD5_TG_PAGE_HILL. That page is built by
 * tg_emit_texture_page_fb_terrain(which=1), which paints a pale near-white
 * SNOWLINE over its top ~10..14 rows and cool grey rock below -- so every quad
 * drawn on it carries a light horizontal band across a grey field. That is the
 * "gray-ish stripe", and it is there in FOREST at span 294 exactly as it is on a
 * peak, because the page is chosen per PIECE and never per span.
 *
 * The page was authored as the DISTANT hillside, and a snowline is right for a
 * distant summit. It stopped being distant at R3, when the far ridge was moved
 * off it (see the R3 item 8 note on the ridge) and the tunnel massing became its
 * only consumer -- a flank standing at ROAD level, metres from the camera, in
 * whatever biome the mouth happens to sit in.
 *
 * Fix by the piece's own property rather than by a page blacklist: a shoulder IS
 * the hillside the bore is cut into, and the hillside it seams to is the ground
 * the terrain pass already laid at the mouth, so read THAT page -- the R9 TOPO
 * C4 rule the far-band apron follows. Sampled at the nearest span OUTSIDE the
 * run, because tg_ground_page_for_span deliberately suppresses the snow override
 * on tunnel spans (snow inside a bore would glow through the lining) and the
 * flank is outside. A snow biome therefore gets snow, a green one gets its own
 * ground, and no biome gets a snowline band it did not earn.
 * TD5RE_R11_TUNNEL_FLANK=0 restores TD5_TG_PAGE_HILL for an A/B. */
static int tg_tunnel_flank_page(int si)
{
    int d;

    if (!td5_env_flag_on("TD5RE_R11_TUNNEL_FLANK")) return TD5_TG_PAGE_HILL;
    /* Walk out to the first span that is not in the bore. The massing only ever
     * runs TD5_TG_TUNNEL_MASS_SPANS from a mouth, so this is a short walk. */
    for (d = 0; d <= TD5_TG_TUNNEL_MASS_SPANS + 1; d++) {
        if (!tg_span_in_tunnel(si - d)) return tg_topo_surface_page(si - d);
        if (!tg_span_in_tunnel(si + d)) return tg_topo_surface_page(si + d);
    }
    return tg_topo_surface_page(si);
}

/* Group C -- tunnels: portal surrounds, mountain massing, width for branches.
 * Called INSIDE the tunnel branch, where no buildings/props are emitted.
 *
 * ROOT CAUSE this addresses: tg_emit_tunnel emits an enclosure and nothing
 * else, so a tunnel in open country is a concrete box sitting ON the landscape
 * with sky above and behind it -- the road does not go THROUGH anything, so the
 * mouth reads as the end of a shed. Fix by putting mass over and beside the
 * bore: a CROWN slab stacked on the roof and a SHOULDER each side, plus, at the
 * portal span itself, two BUTTRESSES hugging the mouth so the opening reads as
 * cut into rock. Because this hook only runs on tunnel spans, the mountain
 * begins exactly at the mouth -- which is what you want: from outside you see a
 * rock face with a hole in it.
 *
 * Weighted toward the PORTALS, and skipped past TD5_TG_TUNNEL_MASS_SPANS, for
 * the reason above: deep inside the run the mass is occluded by the lining, so
 * emitting it there would only spend mesh budget.
 *
 * Pages: TD5_TG_PAGE_HILL, read-only (Group D fills it) -- a hillside flank is
 * exactly what this is. Vertex colour darkens the mass slightly so it does not
 * read as the same material as the lining. */
int tg_emit_fb_tunnel(const TG_FBHook *h)
{
    const TG_Node *n = &h->nl->v[h->si];
    const double lx = n->tz, lz = -n->tx;
    const double roof_top = n->y + TD5_TG_TUNNEL_HEIGHT + 400.0;
    const unsigned int rock  = 0xFFC0C8C0u;   /* lit rock/turf flank */
    const unsigned int shade = 0xFF98A098u;   /* the cut face at the mouth */
    double bore_half, bore_shift, side_x, cx, cz, vis, crown_h, flank_hx;
    int ed, s;
    /* ==================================================================
     * [R9 TUNNEL item 5a]  THE OBJECT THREE ROUNDS MISSED.
     *
     * "the entrance of the tunnel still have wrong textures on top and on its
     *  sides" -- fourth round on this sentence.
     *
     * R6 built a portal page, R7 built a projecting concrete portal facade, R8
     * replaced the banded page with a flat one and verified the SELECTION at all
     * twelve mouths. Every one of those was true and none of them touched the
     * thing being complained about, because "on top" and "on its sides" are NOT
     * the portal facade. They are the CROWN slab and the two BUTTRESSES emitted
     * right here, twenty spans away in the source and on a completely different
     * page: TD5_TG_PAGE_HILL, the distant-hillside terrain page. The band above
     * the lintel and the wings either side have been mottled green-grey TERRAIN
     * this whole time. The portal read wrong because the hill was wearing the
     * portal's silhouette, and no portal art could ever have fixed that.
     *
     * At the mouth (ed <= 1) the crown and the buttresses are not hillside, they
     * are the CUT FACE and the REVETMENT of the portal, so give them portal
     * masonry. Deeper in (ed > 1) the crown genuinely is the hill going over the
     * top and keeps TD5_TG_PAGE_HILL -- the SHOULDERS keep it at every depth for
     * the same reason: they are the hillside, and they are the pieces the
     * complaint does NOT name. TD5RE_R9_PORTAL_SURROUND=0 puts the whole lot
     * back on the terrain page for the A/B. */
    const int surr_on = td5_env_flag_on("TD5RE_R9_PORTAL_SURROUND");
    const int mouth_pg = surr_on ? TD5_TG_PAGE_R9_PORTAL_SURR : TD5_TG_PAGE_HILL;
    /* Masonry reads as masonry, not as a tinted hillside: the HILL page is
     * green-biased and the rock/shade vertex colours were chosen to sit on it. */
    const unsigned int mrock  = surr_on ? 0xFFD6D2CAu : rock;
    const unsigned int mshade = surr_on ? 0xFFB4B0A8u : shade;
    /* [R11 item 2] The hillside pieces read the ground page of the terrain they
     * meet instead of the snowline-banded TD5_TG_PAGE_HILL. See the helper. */
    const int flank_pg = tg_tunnel_flank_page(h->si);

    /* [R16 TUNNEL item a/b/c] FOREST WRAP. In a forested wilderness biome
     * (FOREST/ALPINE: billboard trees, urbanity 0) the mountain massing is the
     * WRONG answer to "a bare tunnel in open country reads as a shed": there is
     * no open country here, there is FOREST, and the tree line beside the mouth
     * is what should wrap the portal. Instead the crown/shoulders/buttresses
     * stacked rock boxes (p371 hillside flanks -- "these boxes should be deleted")
     * and PORTAL_SURROUND masonry (p444 -- "is also not necessary") IN FRONT of
     * that tree line, reading as several facades and hiding the forest the user
     * wants to see wrap the entrance. Suppress the whole massing in these biomes
     * so only the swept portal facade remains ("keep one tunnel facade") and the
     * neighbouring tree line reads as the surround. Non-forest biomes (open
     * FIELDS, edge-of-town, city underpasses) keep the massing -- there the "hole
     * in a shed" reading is real and the rock face is what fixes it.
     * TD5RE_R16_TUNNEL_FOREST_WRAP=0 restores the massing for an A/B. */
    if (td5_env_flag_on("TD5RE_R16_TUNNEL_FOREST_WRAP") &&
        h->b->billboard && h->b->urbanity == 0)
        return 1;

    /* Default ON (a fix); TD5RE_AUTOTRACK_TUNNEL_MOUNTAIN=0 to disable. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_MOUNTAIN")) return 1;

    ed = tg_tunnel_edge_dist(h->si, TD5_TG_TUNNEL_MASS_SPANS);
    if (ed > TD5_TG_TUNNEL_MASS_SPANS) return 1;      /* buried out of sight */
    if (*h->nmesh + 6 > h->maxmesh) return 1;         /* budget, not an error */

    /* 1.0 at the portal span, tapering inward -- the hill thins as it goes
     * behind the lining, which is also how a real cutting looks from outside. */
    vis = (double)(TD5_TG_TUNNEL_MASS_SPANS + 1 - ed) /
          (double)TD5_TG_TUNNEL_MASS_SPANS;

    tg_tunnel_bore(h->nl, h->si, &bore_half, &bore_shift);
    side_x = bore_half + TD5_TG_TUNNEL_WALL_T;
    cx = n->x + lx * bore_shift;
    cz = n->z + lz * bore_shift;

    crown_h  = 700.0 + 1900.0 * vis;
    flank_hx = 900.0 + 2200.0 * vis;

    /* CROWN: mass stacked directly on the roof slab, overhanging it a little so
     * no sliver of sky shows between hill and tunnel.
     * [R9 item 5a] AT THE MOUTH this is the band directly above the portal
     * lintel -- "wrong textures on top" -- so it is portal masonry there and
     * hillside only once it is behind the facade. */
    h->moff[(*h->nmesh)++] = h->blk->len;
    if (!tg_emit_box_mesh(h->blk, cx, roof_top + crown_h * 0.5, cz,
                          side_x + 900.0, crown_h * 0.5, 780.0,
                          n->tx, n->tz,
                          (ed <= 1) ? mouth_pg : flank_pg, 3000.0,
                          (ed <= 1) ? mrock : rock))
        return 0;

    /* SHOULDERS: one each side, outboard of the roof overhang so they never
     * intrude into the bore, sunk well below the road so they meet whatever
     * terrain the ground pass laid down instead of floating over it. */
    for (s = 0; s < 2; s++) {
        const double sgn = s ? 1.0 : -1.0;
        const double off = side_x + 900.0 + flank_hx;
        const double top = roof_top - 600.0;
        const double bot = n->y - 2500.0;
        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_emit_box_mesh(h->blk, cx + lx * off * sgn, (top + bot) * 0.5,
                              cz + lz * off * sgn,
                              flank_hx, (top - bot) * 0.5, 780.0,
                              n->tx, n->tz, flank_pg, 3000.0, rock))
            return 0;
    }

    /* BUTTRESSES: only on the portal span itself. Deeper along the road than a
     * span slab (1400 vs 780) so the mouth sits in a recess rather than flush
     * with the hillside, which is what makes it read as a bore rather than a
     * hole painted on a wall.
     *
     * [R9 item 5a] These are the "sides" of the complaint -- the wings either
     * side of the opening -- and they were terrain. They are now revetment.
     * [R9 item 5b] Also deepened 1400 -> 1400 + the portal's proud offset, so
     * the buttress reaches the frame plane instead of stopping 1600 short of it.
     * A buttress that ends behind the facade is exactly the "thin slab with no
     * reveal" reading; one that runs out to the facade gives the mouth a jamb
     * with visible thickness on both sides of the reveal. */
    if (ed <= 1) {
        /* [R12 item 6] 2200 stood the pillar's front face 600 in FRONT of the
         * frame plane, so the beam was recessed behind the pillars it sits on --
         * the reported gap. Pull it back to just behind the frame, which is
         * what R9 item 5b asked for and what the overshoot broke. */
        const double bdepth = surr_on ? tg_portal_butt_deep() : 1400.0;
        for (s = 0; s < 2; s++) {
            const double sgn = s ? 1.0 : -1.0;
            const double off = side_x + TD5_TG_BUTT_INSET;
            if (*h->nmesh + 1 > h->maxmesh) break;
            h->moff[(*h->nmesh)++] = h->blk->len;
            if (!tg_emit_box_mesh(h->blk, cx + lx * off * sgn, n->y + 900.0,
                                  cz + lz * off * sgn,
                                  TD5_TG_BUTT_HALFW, 2400.0, bdepth,
                                  n->tx, n->tz, mouth_pg, 2400.0,
                                  mshade))
                return 0;
        }
    }
    return 1;
}

/* [R11 item 7a] The band's reach as ONE number, knob included. The overpass
 * sizes its arms off this so "the end of the drawn area" means the same thing
 * to the highway and to the ground it is drawn over.
 *
 * [R11 integration] tg_r11_wet_reach below reads it too, so the shoreline, the
 * river and the overpass arms all take "the drawn edge" from this one place. */
double tg_far_reach(void)
{
    return (double)td5_env_int("TD5RE_AUTOTRACK_TERRAIN_REACH",
                               TD5_TG_FAR_REACH, 30000, 400000);
}

/* [R11 WATER item 14] How far out from the centreline the river and its
 * coastline reach beside node si.
 *
 * "The coastline should reach the end of the drawn area" needs a definition of
 * that end, and the far band above IS it: off a bridge run it is the outermost
 * ground on the side, and the reach knob moves it. R9 sized the river and the
 * shore band to TD5_TG_BRIDGE_WATER_HALF instead, an unrelated gorge constant,
 * so where a bridge run abuts a group that still draws its band the water
 * stopped 1000 units inside the terrain behind it and the rim read as an open
 * edge. Reading the band's own reach ties the two together, so a later change to
 * TD5RE_AUTOTRACK_TERRAIN_REACH moves the shoreline with the horizon instead of
 * silently reopening this.
 *
 * Never SHRINKS the footprint (max with the old half-width), so no existing
 * water gets narrower, and it is a purely lateral quantity -- no caller derives
 * a height from it, which is what keeps piers, the submerged gorge skirt and the
 * relief system out of its blast radius. TD5RE_R11_WATER=0 pins it to the R9
 * constant. */
double tg_r11_wet_reach(const TG_NodeList *nl, int si)
{
    const double bw = TD5_TG_BRIDGE_WATER_HALF;
    double r;

    if (!td5_env_flag_on("TD5RE_R11_WATER")) return bw;
    if (si < 0) si = 0;
    if (si > nl->count - 1) si = nl->count - 1;
    r = nl->v[si].width * 0.5 + tg_far_reach();
    return r > bw ? r : bw;
}

/* THE CURE for the ceiling above, replacing the reach cut that only hid it.
 *
 * The band was flat: all four of its rings sat at the emitting span's own road
 * height, so a band on high ground was a slab of high ground hanging over
 * whatever low ground it reached. Height is a LOCAL fact but the band's extent
 * is not, which is the whole defect -- and the same shape as the 2026-08-24
 * elevated-bridge gate, one axis over.
 *
 * So make it a SLOPE instead of a slab. Ring 0 still meets the skirt at the
 * local road height, because that seam has to stay watertight; from there the
 * band descends to the track's GLOBAL minimum (tg_track_min_y) and stays there
 * for the outer two rings. Two consequences, and both are the point:
 *   - beyond ring 2 every band on the track is at the SAME height, so however
 *     many of them overlap out there they are coplanar, not stacked;
 *   - nothing above the global floor survives past 45% of the reach, so the
 *     span of track a band can hang over shrinks from the full reach to that.
 * On the measured seed 1234567 (relief 10662, reach 30000) that is the
 * difference between a 6400-unit ceiling and none.
 *
 * TD5RE_AUTOTRACK_TERRAIN_SINK=0 restores the flat slab. */
static const double k_tg_far_sink[4] = { 0.0, 0.45, 1.0, 1.0 };

/* Default ON -- these are fixes. TERRAIN_FAR=0 restores the short skirt only,
 * TERRAIN_HILLS=0 keeps the long plain but drops the distant ridge wall. */
static int tg_terrain_far_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_FAR");
}

static int tg_terrain_ridge_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_HILLS");
}

/* [R4 item 5] Two independent far-ridge fixes, each its own single-variable
 * flag (both default ON):
 *  - TREELINE_FIX: half-texel V inset that kills the "black line at the top".
 *    Safe by construction (matches the facade UV-inset precedent).
 *  - CITY_SKYLINE: route the ridge to a blocky building skyline in urban biomes
 *    instead of a forest canopy ("which ones are suitable for cities"). A
 *    judgment change, split out so it can be A/B'd and reverted on its own. */
static int tg_r4_treeline_fix(void)
{
    return td5_env_flag_on("TD5RE_R4_TREELINE_FIX");
}

static int tg_r4_city_skyline(void)
{
    return td5_env_flag_on("TD5RE_R4_CITY_SKYLINE");
}

/* [R5 item 16] Tree-line stretch + "height not following the grass" fix. Default
 * ON; TD5RE_R5_FLORA_TREELINE=0 restores the pre-fix band for an A/B. */
static int tg_r5_treeline_fix(void)
{
    return td5_env_flag_on("TD5RE_R5_FLORA_TREELINE");
}

/* [R8 TERRAIN item 14] "the background texture for tree lines ... looks
 * STRETCHED and VERY REPEATED, is the texture bigger than one span? should you
 * use more texture variety?"
 *
 * The user's own hypothesis, checked rather than assumed, and it is half right.
 * MEASURED against the shipped R7 ridge:
 *   - horizontal: R5 item 16 already tiles the ridge by its ACTUAL world width
 *     at one tile per TD5_TG_SPAN_LENGTH, so a tile is 1500 world units wide --
 *     exactly one span, not bigger.
 *   - vertical: v runs 0..1 over the WHOLE wall in one go, and the wall is
 *     TD5_TG_RIDGE_BASE (4500) plus up to 3600 of hill roll, floored at
 *     TD5_TG_RIDGE_MIN_UP (1200) and lifted further to clear the road it is
 *     seen from. So one 64x64 tile is drawn 1500 wide by ~4500 tall.
 * That is a 1:3 vertical stretch of a square page, which is the "stretched"
 * half of the report. The "very repeated" half is a separate cause: the ridge
 * has always drawn on ONE page for the entire track, and every far-group
 * restarts u at 0, so the same four tiles recur every four spans forever.
 *
 * Two independent fixes, two knobs, because they are two defects:
 *   ASPECT: tile horizontally by the wall's own crest height instead of by the
 *     span length, so a tile is as wide as it is tall and the page is drawn at
 *     the ratio it was authored at.
 *   VARY:  four tree-line pages instead of one, picked per far-group, plus a
 *     per-group integer u phase so the tile sequence does not restart at the
 *     same texel column. The phase is an INTEGER number of tiles, so the page
 *     still wraps exactly and group-to-group joins stay continuous. */
static int tg_r8_treeline_aspect(void)
{
    return td5_env_flag_on("TD5RE_R8_TERRAIN_TREELINE_ASPECT");
}

/* [R15 BAND item 3] "the texture of the city behind is too stretched out and
 * barely noticeable."
 *
 * The R5 item 16 world-width tiling below fixed exactly this complaint for the
 * TREE LINE, but its gate is `r5treeline` = non-snow AND urbanity < 2, so the
 * URBAN skyline ridge -- the one branch whose page IS a city -- was left on the
 * old fixed 4-tile U. Same quad, same TD5_TG_FAR_REACH (30000) fan, same
 * thousands-of-units-per-tile stretch; only the page differs. This extends the
 * existing fix to that branch rather than inventing a second rule. */
static int tg_r15_skyline_uv(void)
{
    return td5_env_flag_on("TD5RE_R15_SKYLINE_UV");
}

static int tg_r8_treeline_vary(void)
{
    return td5_env_flag_on("TD5RE_R8_TERRAIN_TREELINE_VARY");
}

/* Which tree-line page a far-group draws on. Keyed on the group's FIRST span so
 * the two ends of one quad can never disagree, and hashed rather than taken
 * modulo so neighbouring groups do not walk the variants in a visible cycle. */
/* [R8 item 14] Measured tiling of every tree-line ridge actually emitted, so
 * "stretched" and "repeated" are reported as numbers rather than judged from a
 * screenshot. tile_w / tile_h are the WORLD size one 64x64 page tile is drawn
 * at; their ratio is the stretch. pages[] counts how many ridges landed on each
 * variant slot. Reported by tg_r8_terrain_extent_report. */
static double s_r8_tl_w, s_r8_tl_h, s_r8_tl_ratio_min, s_r8_tl_ratio_max;

static int    s_r8_tl_n, s_r8_tl_pages[TD5_TG_R8_TREELINE_N + 1];

static void tg_r8_tl_note(double tile_w, double tile_h, int page)
{
    const double r = (tile_h > 1.0) ? tile_w / tile_h : 0.0;
    if (!s_r8_tl_n) { s_r8_tl_ratio_min = r; s_r8_tl_ratio_max = r; }
    if (r < s_r8_tl_ratio_min) s_r8_tl_ratio_min = r;
    if (r > s_r8_tl_ratio_max) s_r8_tl_ratio_max = r;
    s_r8_tl_w += tile_w; s_r8_tl_h += tile_h; s_r8_tl_n++;
    if (page >= TD5_TG_PAGE_R8_TREELINE &&
        page <  TD5_TG_PAGE_R8_TREELINE + TD5_TG_R8_TREELINE_N)
        s_r8_tl_pages[page - TD5_TG_PAGE_R8_TREELINE]++;
    else
        s_r8_tl_pages[TD5_TG_R8_TREELINE_N]++;   /* the single legacy page */
}

/* [R15 BAND item 3] The urban skyline's own tile-size census. Deliberately NOT
 * folded into tg_r8_tl_note above: that one histograms by TREELINE page id, so
 * every skyline sample would land in its "legacy page" bucket and drag the
 * tree-line ratio min/max with it. Separate counters keep both reports honest,
 * and give this round the before/after number for "too stretched out". */
double s_r15_sky_w, s_r15_sky_h, s_r15_sky_ratio_min, s_r15_sky_ratio_max;
int    s_r15_sky_n;

void tg_r15_sky_note(double tile_w, double tile_h)
{
    const double r = (tile_h > 1.0) ? tile_w / tile_h : 0.0;
    if (!s_r15_sky_n) { s_r15_sky_ratio_min = r; s_r15_sky_ratio_max = r; }
    if (r < s_r15_sky_ratio_min) s_r15_sky_ratio_min = r;
    if (r > s_r15_sky_ratio_max) s_r15_sky_ratio_max = r;
    s_r15_sky_w += tile_w; s_r15_sky_h += tile_h; s_r15_sky_n++;
}

void tg_r15_sky_report(void)
{
    if (s_r15_sky_n)
        TD5_LOG_I(LOG_TAG, "[R15 BAND item 3] urban skyline ridge: %d quad(s), "
                  "mean tile %.0f x %.0f world units, aspect %.2f..%.2f (knob "
                  "TD5RE_R15_SKYLINE_UV=%s)", s_r15_sky_n,
                  s_r15_sky_w / (double)s_r15_sky_n,
                  s_r15_sky_h / (double)s_r15_sky_n,
                  s_r15_sky_ratio_min, s_r15_sky_ratio_max,
                  tg_r15_skyline_uv() ? "on" : "off");
}

int tg_r8_treeline_page(int g0)
{
    unsigned int h;
    if (!tg_r8_treeline_vary()) return TD5_TG_PAGE_TREELINE;
    h = (unsigned)(g0 / TD5_TG_FAR_GROUP) * 2654435761u;
    return TD5_TG_PAGE_R8_TREELINE
         + (int)((h >> 13) % (unsigned)TD5_TG_R8_TREELINE_N);
}

/* Smooth low-frequency terrain height at a world point. Two summed products of
 * sines at ~42000 and ~17000 world units, which at a 1500-unit span length is a
 * hill every ~28 and ~11 spans -- long enough to read as topology from a car
 * rather than as noise. Result is in [-amp, +amp]. */
static double tg_terrain_hill_y(double x, double z, double amp)
{
    const double s1 = 1.0 / 42000.0, s2 = 1.0 / 17000.0;
    return amp * (0.70 * sin(x * s1 + 1.3) * cos(z * s1 - 0.4)
                + 0.30 * sin(x * s2 - 2.1) * cos(z * s2 + 0.9));
}

/* Does span si emit its group's band? The FIRST non-tunnel span of the group
 * does. The hook is not called on tunnel spans at all, so keying on si % GROUP
 * would silently drop a band wherever a tunnel run happened to cover the
 * group's first span. */
static int tg_far_group_owner(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    int j;
    for (j = g0; j < si; j++)
        if (!tg_span_in_tunnel(j)) return 0;
    return 1;
}

static int tg_r9_dryband_enabled(void)
{
    return td5_env_flag_on("TD5RE_R9_BRIDGE_DRYBAND");
}

/* Class-level evidence for item 10: how many band sides were clamped, by how
 * much, and how many were dropped outright. Reported by tg_r9_bridge_report. */
static int    s_r9_band_tested, s_r9_band_clamped, s_r9_band_dropped;

static double s_r9_band_clamp_sum;

/* Is world point (wx,wz) inside the river rectangle of any bridge run near
 * span si0? Same rectangle tg_emit_bridge_water lays, so the two cannot
 * disagree about where the water is. */
/* [R14 GENPERF] WHICH SPANS CARRY RIVER, decided once for a whole sample sweep.
 * The window scan below is a function of si0 alone -- the sample point never
 * enters it -- yet it used to run inside the point test, so tg_r9_dry_reach
 * re-derived the same 91-span answer for each of its 41 samples: 3731
 * tg_span_in_bridge_run + tg_water_span_clear pairs per call, measured at 43%
 * of the entire generation. Collected once here and handed to the point test.
 * Ascending order is preserved, so the point test still returns on the same
 * first match it always did. */
static int tg_r9_wet_window_spans(const TG_NodeList *nl, int si0, int *out)
{
    int s, lo = si0 - TD5_TG_R9_WATER_WINDOW, hi = si0 + TD5_TG_R9_WATER_WINDOW;
    int n = 0;
    if (lo < 0) lo = 0;
    if (hi > nl->count - 2) hi = nl->count - 2;
    for (s = lo; s <= hi; s++)
        if (tg_span_in_bridge_run(s) && tg_water_span_clear(s)) out[n++] = s;
    return n;
}

static int tg_r9_point_over_bridge_water(const TG_NodeList *nl,
                                         const int *wet, int nwet,
                                         double wx, double wz)
{
    const double BW = TD5_TG_BRIDGE_WATER_HALF + TD5_TG_R9_WATER_MARGIN;
    int i;
    for (i = 0; i < nwet; i++) {
        const int s = wet[i];
        const TG_Node *n0, *n1;
        double dx, dz, along, lat, ax, az, len;
        n0 = &nl->v[s]; n1 = &nl->v[s + 1];
        dx = wx - n0->x; dz = wz - n0->z;
        /* tg_emit_bridge_water's own axes: left unit is (tz, -tx). */
        along = dx * n0->tx + dz * n0->tz;
        lat   = dx * n0->tz - dz * n0->tx;
        if (lat <= -BW || lat >= BW) continue;
        ax = n1->x - n0->x; az = n1->z - n0->z;
        len = sqrt(ax * ax + az * az);
        if (along < -TD5_TG_R9_WATER_MARGIN ||
            along > len + TD5_TG_R9_WATER_MARGIN) continue;
        return 1;
    }
    return 0;
}

/* Largest outward distance from (ox,oz) along unit (ux,uz) that stays off the
 * river. Sampled rather than solved: the band is four rings, so a sample grid
 * finer than the ring spacing is all the resolution the result can carry. */
static double tg_r9_dry_reach(const TG_NodeList *nl, int si,
                              double ox, double oz, double ux, double uz,
                              double d0, double dmax)
{
    const int N = 40;
    int wet[2 * TD5_TG_R9_WATER_WINDOW + 1];
    int k, nwet;
    if (dmax <= d0) return dmax;
    /* [R14 GENPERF] ONCE per sweep, not once per sample. With no river span in
     * the window at all -- which is the common case, since a bridge is a rare
     * feature and this runs on every band on the track -- nwet is 0, every
     * sample below answers "dry" without touching a node, and the whole sweep
     * collapses to the loop counter. That is the same answer the old code
     * reached by scanning 91 spans 41 times over. */
    nwet = tg_r9_wet_window_spans(nl, si, wet);
    for (k = 0; k <= N; k++) {
        const double d = d0 + (dmax - d0) * (double)k / (double)N;
        if (tg_r9_point_over_bridge_water(nl, wet, nwet,
                                          ox + ux * d, oz + uz * d))
            return d0 + (dmax - d0) * (double)(k > 0 ? k - 1 : 0) / (double)N;
    }
    return dmax;
}

static int tg_r13_band_cull(void)
{
    return td5_env_flag_on("TD5RE_R13_BAND_CULL");   /* default ON */
}

/* Class-level ledger. Counted on EVERY run, acted on only when the knob is on,
 * so the report-only run measures the exact bytes the acting run removes. */
static long s_r13_far_meshes, s_r13_far_bytes;   /* far bands actually written */

static long s_r13_far_hidden, s_r13_far_hidden_bytes;  /* whole mesh invisible */

static long s_r13_far_ridge;                     /* crest hidden, apron kept   */

static long s_r13_far_skyline_kept;              /* the buildings carve-out    */

/* One side's background band: 3 ground quads outward plus the ridge wall.
 *
 * [R8 TERRAIN items 5/15] `ridge_ok` = 0 emits the GROUND apron without the
 * standing ridge wall. See tg_emit_fb_terrain for why that split exists. */
static int tg_emit_far_band(const TG_FBHook *h, int is_left, int ridge_ok)
{
    const TG_NodeList *nl = h->nl;
    const double base_reach = tg_far_reach();
    double reach = base_reach;
    /* [R9 TOPO C2/C3] This band's own reach, decided by the continuity rule
     * rather than by a single global constant. Filled in below once the seam
     * height and the skirt's outer distance are known -- see the RUN-OUT note. */
    int runout = 0;
    /* Band height envelope: flat at the seam, rolling further out. */
    static const double k_amp[4] = { 0.0, 700.0, 2200.0, 4200.0 };
    const int sink = td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_SINK");
    const double floor_y = tg_track_min_y(nl) - TD5_TG_FAR_SINK_AT;
    const int g0 = (h->si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    double so_max = 0.0;
    double X[2][4], Y[2][4], Z[2][4], D[2][4], U[2], B[2];
    double px[16], py[16], pz[16], uu[16], vv[16];
    int seg_page[2], seg_nq[2];
    int e, j, n = 0, nseg = 1;
    /* [R5 item 16] The height/stretch cure only applies where the ridge is an
     * actual tree line: non-snow, non-urban (snow keeps its flank, urban gets the
     * blocky skyline -- see the seg_page[1] routing below). */
    const int r5fix = tg_r5_treeline_fix();
    const int r5treeline = !tg_biome_is_snow(h->b) && h->b->urbanity < 2;
    /* [R9 item 10] Per-side dry reach, decided across BOTH ends so the band
     * stays a proper quad; see tg_r9_dry_reach. */
    double band_reach = reach;
    /* [R13 BAND item 1b] Set on a report-only run when this mesh was judged
     * wholly invisible, so its bytes can be billed after it is written. */
    int r13_hidden = 0;

    if (g1 > nl->count - 2) g1 = nl->count - 2;
    if (g1 < g0) return 1;

    /* ---------------- [R9 TOPO C2/C3] RUN-OUT AND ROAD CAP ----------------
     *
     * The reach used to be one global constant for every band on the track.
     * That is fine while a wall stands on the outer edge -- the wall closes the
     * view and the edge is never seen. Where the RIDGE IS SUPPRESSED (the fork
     * and tunnel gates, which R8 TERRAIN correctly turned into ridge-only gates
     * so the ground survived) the band ends in an open edge instead, and
     * because the R8 sink descends it toward the track's global floor on the
     * way out, that edge has FALLEN by the time it stops. Falling ground is not
     * foreshortened -- you look down onto it -- so its terminating edge is
     * plainly in view with sky beyond. That is seed 99991 span 549 R: inside
     * fork 3 (510-631), ground measured at the full 30000 by R8's own report,
     * and still "no geometry on the right side" because the last thing you see
     * of it is a cut edge on a slope.
     *
     * So a band with no wall must RUN OUT: keep going until its terminal grade
     * is TD5_TG_TOPO_RUNOUT, which for a drop of D costs D/0.06 of extra
     * horizontal. That is the user's "if you make a sloped side make sure that
     * it goes further than usual", stated as arithmetic and scaled by the
     * actual slope. Safe by the sink's own argument: the extension only
     * lengthens rings 2 and 3, which sit ON the global floor and are therefore
     * coplanar with every other band out there, so no amount of overlap can
     * stack them. The two outer rings are also pinned to the floor here
     * (the R5 tree-line shallow drop is skipped when running out) precisely so
     * that argument keeps holding at the longer reach.
     *
     * C3 then caps whatever C2 asked for at the nearest OTHER carriageway, so
     * running out can never walk this band across the far leg of a U-turn.
     */
    if (tg_topo_enabled()) {
        double cap = 1e30, drop_max = 0.0;
        for (e = 0; e < 2; e++) {
            const int se = e ? g1 : g0;
            const double wsd = h->b->water ? tg_water_side(se) : 0.0;
            const double c = TG_SUB3D(TG_SUB3_ROADCAP,
                                      tg_topo_road_cap(nl, se, is_left));
            TG_GroundProf pp;
            double seam, d;
            TG_SUB3V(TG_SUB3_GROUNDSIDE, tg_ground_side(nl, se, is_left, wsd, &pp));
            seam = nl->v[se].y - pp.dy[pp.n - 1] - TD5_TG_FAR_SINK;
            d = seam - floor_y;
            if (d > drop_max) drop_max = d;
            if (c < cap) cap = c;
            if (pp.d[pp.n - 1] > so_max) so_max = pp.d[pp.n - 1];
        }
        /* "No wall" means no wall ACTUALLY EMITTED -- ridge_ok is only half of
         * that test, and tg_topo_chain's closure test is the other half, so
         * they are written the same way here and there. */
        if (!(ridge_ok && tg_terrain_ridge_enabled()) && sink) {
            const double want = tg_topo_runout_reach(so_max, drop_max, base_reach);
            if (want > reach) { reach = want; runout = 1; }
        }
        /* C3 applies to EVERY band, walled or not: ground belonging to this
         * span must not be laid across another road at this span's height. */
        if (cap < reach) reach = cap;
        if (reach < so_max + TD5_TG_FAR_TUCK + 500.0)
            reach = so_max + TD5_TG_FAR_TUCK + 500.0;
    }

    /* [R17 TERRAIN item 2] "this background is crossing through the middle of the
     * road." The C3 road cap (and the C2 run-out and the min-width floor) above
     * all land in `reach`, but the apron rings below are built from `band_reach`,
     * which was snapshotted from the UNCAPPED reach at the top and is only ever
     * lowered further down by the R9 water dry-band clamp. On a DRY hairpin /
     * U-turn the cap was therefore computed and thrown away, and the apron ran
     * the full uncapped reach straight across the opposing carriageway. Carry the
     * capped reach into band_reach so the ground footprint honours the same road
     * cap the ridge already does; the water clamp below still lowers it further
     * where a river is nearer (it re-reads `reach`, not band_reach, so order is
     * unaffected). RESIDUAL (unfixed here, kept tight): the cap samples only the
     * two group ENDS (g0,g1), so a hairpin apex between them is missed, and the
     * min-width floor at :3317 can still nudge the outer ring a few hundred units
     * past a carriageway that sits inside the skirt. TD5RE_R17_FARBAND_ROADCAP=0
     * restores the uncapped apron for an A/B. */
    if (td5_env_flag_on("TD5RE_R17_FARBAND_ROADCAP"))
        band_reach = reach;

    /* [R9 merge] TOPO's run-out EXTENDS reach; BRIDGE's dry-band CLAMPS it off
     * water. They are independent and both wanted, but the ORDER is load-bearing:
     * extend first, then clamp, so a band that ran out over a river is still
     * pulled back, while a band nowhere near water keeps the full run-out that
     * fixes item 6. Reversing them would let the clamp silently undo the run-out.
     *
     * BRIDGE's own report notes this clamp is NOT what fixed item 10 (the far
     * band was never over the river -- tg_emit_far_shore was); it is kept to
     * enforce the rule for a future crossing that does reach. So it must stay
     * subordinate to the run-out, not compete with it.
     *
     * Its local so_max is renamed dry_so_max: the enclosing scope already has a
     * so_max that the TOPO block above reads, and shadowing it would trip
     * -Wshadow and the warnings ratchet. */
    if (tg_r9_dryband_enabled()) {
        double lim = reach, dry_so_max = 0.0;
        s_r9_band_tested++;
        for (e = 0; e < 2; e++) {
            const int se = e ? g1 : g0;
            double lx, ly, lz, rx, ry, rz, ux, uz, len, so, d;
            TG_GroundProf p;
            TG_SUB3V(TG_SUB3_ROADEDGE,
                     tg_road_edge(nl, se, e ? 1.0 : 0.0, 0.0, 1.0,
                                  &lx, &ly, &lz, &rx, &ry, &rz));
            ux = lx - rx; uz = lz - rz;
            len = sqrt(ux * ux + uz * uz);
            if (len < 1e-6) { ux = 1.0; uz = 0.0; } else { ux /= len; uz /= len; }
            if (!is_left) { ux = -ux; uz = -uz; }
            TG_SUB3V(TG_SUB3_GROUNDSIDE,
                     tg_ground_side(nl, se, is_left,
                                    h->b->water ? tg_water_side(se) : 0.0, &p));
            so = p.d[p.n - 1];
            if (so > dry_so_max) dry_so_max = so;
            d = TG_SUB3D(TG_SUB3_DRYREACH,
                         tg_r9_dry_reach(nl, h->si, (is_left ? lx : rx),
                                         (is_left ? lz : rz), ux, uz,
                                         so - TD5_TG_FAR_TUCK, reach));
            if (d < lim) lim = d;
        }
        if (lim < reach) {
            /* The skirt itself already ends in the river: there is no dry ground
             * out here to draw, so draw none. */
            if (lim <= dry_so_max + TD5_TG_R9_WATER_MARGIN) {
                s_r9_band_dropped++;
                return 1;
            }
            s_r9_band_clamped++;
            s_r9_band_clamp_sum += reach - lim;
            band_reach = lim;
        }
    }

    for (e = 0; e < 2; e++) {
        const int se = e ? g1 : g0;
        double lx, ly, lz, rx, ry, rz, ux, uz, len, so, base;
        TG_GroundProf p;

        tg_road_edge(nl, se, e ? 1.0 : 0.0, 0.0, 1.0,
                     &lx, &ly, &lz, &rx, &ry, &rz);
        ux = lx - rx; uz = lz - rz;
        len = sqrt(ux * ux + uz * uz);
        if (len < 1e-6) { ux = 1.0; uz = 0.0; } else { ux /= len; uz /= len; }
        if (!is_left) { ux = -ux; uz = -uz; }

        /* Start where the skirt ended, from the SAME profile the skirt used --
         * including its WATER SIDE. This used to hardcode 0.0 while
         * tg_emit_ground passes the real side, so on a coastal run the band and
         * the skirt disagreed about where the ground was. */
        {
            const double wsd = h->b->water ? tg_water_side(se) : 0.0;
            double drop;

            TG_SUB3V(TG_SUB3_GROUNDSIDE, tg_ground_side(nl, se, is_left, wsd, &p));
            so   = p.d[p.n - 1];
            drop = p.dy[p.n - 1];
            /* Belt and braces on top of the clamp in tg_ground_side: this band
             * runs out to TD5_TG_FAR_REACH (180000), so ANY upward error here is
             * multiplied into a slab across the whole view. Measured on seed
             * 1234567 span 218 before the fix: the seam sat 6350 units ABOVE the
             * road and the player drove under its underside, which read in frame
             * as the sky being replaced by a dark ceiling. The far terrain is
             * never allowed above the road edge. */
            if (drop < 0.0) drop = 0.0;
            base = (is_left ? ly : ry) - drop - TD5_TG_FAR_SINK;
        }
        U[e] = (double)se + (e ? 1.0 : 0.0);
        B[e] = base;

        /* Geometric spacing: each band is roughly twice the depth of the one
         * inside it, so the near ground still has detail while three quads
         * still cover ten times the old reach. */
        D[e][0] = so - TD5_TG_FAR_TUCK;
        D[e][1] = so + (band_reach - so) * 0.18;
        D[e][2] = so + (band_reach - so) * 0.45;
        D[e][3] = band_reach;

        for (j = 0; j < 4; j++) {
            const double ex = (is_left ? lx : rx) + ux * D[e][j];
            const double ez = (is_left ? lz : rz) + uz * D[e][j];
            /* Descend from the seam to the GLOBAL floor -- see k_tg_far_sink.
             * Never the other way: where the emitting span IS the low point of
             * the track the floor is above the seam, and lifting the band onto
             * it would recreate the ceiling this cures. */
            double yb   = sink ? base + (floor_y - base) * k_tg_far_sink[j] : base;
            double ampj = k_amp[j];
            if (yb > base) yb = base;
            /* [R5 item 16] On a tree line, replace the deep floor sink of the two
             * outer points with a shallow, gently-rolling drop below the seam, so
             * the ridge that stands on j==3 follows the near grass instead of
             * dropping into a jagged trough. Kept strictly at or below `base`. */
            /* [R9 TOPO C2] While RUNNING OUT the two outer rings must stay ON
             * the global floor -- see the run-out note. The R5 tree-line
             * shallow drop deliberately holds them just under the seam so the
             * ridge that STANDS on ring 3 follows the near grass; there is no
             * ridge here (that is why we are running out) and at the extended
             * reach a near-seam-height outer ring would be the flat overhead
             * slab the sink exists to prevent. */
            if (r5fix && r5treeline && j >= 2 && !runout) {
                yb    = base - TD5_TG_TREELINE_BASE_DROP * k_tg_far_sink[j];
                ampj *= TD5_TG_TREELINE_AMP_SCALE;
                if (yb > base) yb = base;
            }
            X[e][j] = ex;
            Z[e][j] = ez;
            Y[e][j] = yb + tg_terrain_hill_y(ex, ez, ampj);
            if (r5fix && r5treeline && j >= 2 && !runout && Y[e][j] > base)
                Y[e][j] = base;
        }

        /* [DIAG] Every term that decides how high this band sits, so the large
         * one identifies ITSELF. Three inferred mechanisms for the "ceiling"
         * have already been wrong; this prints the arithmetic instead.
         * TD5RE_AUTOTRACK_FAR_LOG=1. */
        if (td5_env_flag_off("TD5RE_AUTOTRACK_FAR_LOG"))
            TD5_LOG_I(LOG_TAG,
                      "farband si=%d %s e=%d biome=%s | road_y=%.0f edge_y=%.0f "
                      "prof_n=%d prof_dy_last=%.0f base=%.0f floor=%.0f sink=%d "
                      "| Y0=%.0f Y3=%.0f | lift_vs_road=%.0f",
                      h->si, is_left ? "L" : "R", e, h->b->name,
                      nl->v[se].y, (is_left ? ly : ry),
                      p.n, p.dy[p.n - 1], base, floor_y, sink,
                      Y[e][0], Y[e][3], Y[e][0] - nl->v[se].y);
    }

    /* [R13 BAND item 1b] Is this whole mesh standing behind an unbroken tree
     * wall? Evaluated on every run so the report-only run measures the acting
     * run's saving exactly; only ACTED on when the knob is on. */
    {
        double wtop = 0.0, wlat = 0.0;
        if (tg_r13_band_covers(nl, g0, g1, is_left, &wtop, &wlat) &&
            wtop > TD5_TG_R13_EYE_Y && wlat > 1.0) {
            /* Upper bound on the crest, relative to the road it is seen from:
             * the hill roll cannot exceed its own amplitude, and the crest is
             * floored at RIDGE_MIN_UP above that road. Bounded rather than
             * recomputed so the answer cannot drift from the block below. */
            const double dr = (D[0][3] < D[1][3]) ? D[0][3] : D[1][3];
            /* The apron's INNER ring is deliberately TUCKED under the skirt
             * (D[0] = so - FAR_TUCK), so the first apron point the player can
             * actually see is the skirt's own outer edge `so`, not D[0]. Testing
             * against D[0] measured whole=0 on seed 20260901: the FOREST wall at
             * 11000 stands 1000 units OUTSIDE the tucked ring but 1000 units
             * INSIDE the 12000 skirt edge, so every band was scored "apron in
             * front" when in fact its only exposed ground is behind the wall. */
            const double d0 = ((D[0][0] < D[1][0]) ? D[0][0] : D[1][0])
                            + TD5_TG_FAR_TUCK;
            double rtop = TD5_TG_RIDGE_MIN_UP;
            int hides_ridge;
            for (e = 0; e < 2; e++) {
                const int se = e ? g1 : g0;
                const double r = Y[e][3] + TD5_TG_RIDGE_BASE + 3600.0
                               - nl->v[se].y;
                if (r > rtop) rtop = r;
            }
            hides_ridge = dr > 1.0 &&
                (wtop - TD5_TG_R13_EYE_Y) * dr >=
                TD5_TG_R13_OCCL_MARGIN * (rtop - TD5_TG_R13_EYE_Y) * wlat;
            /* The carve-out, stated as code: a group that qualifies must not be
             * one whose ridge is the city skyline. Urban biomes carry no tree
             * line, so this can only fire if a future biome grows both. */
            if (hides_ridge && tg_r4_city_skyline() && h->b->urbanity >= 2) {
                s_r13_far_skyline_kept++;
                hides_ridge = 0;
            }
            if (hides_ridge && wlat < d0) {
                s_r13_far_hidden++;
                if (tg_r13_band_cull()) return 1;   /* wholly behind the wall */
                r13_hidden = 1;         /* report-only: emit, bill the bytes */
            } else if (hides_ridge && ridge_ok && tg_terrain_ridge_enabled()) {
                s_r13_far_ridge++;
                if (tg_r13_band_cull()) ridge_ok = 0;
            }
        }
    }

    /* Apron quads, same ring order as the skirt (near-in, near-out, far-out,
     * far-in) so the winding matches geometry that is known to draw. U is the
     * outward distance in span-lengths, matching the skirt's square tiling. */
    for (j = 0; j < 3; j++) {
        px[n]=X[0][j];   py[n]=Y[0][j];   pz[n]=Z[0][j];
        uu[n]=D[0][j]  /(double)TD5_TG_SPAN_LENGTH; vv[n]=U[0]; n++;
        px[n]=X[0][j+1]; py[n]=Y[0][j+1]; pz[n]=Z[0][j+1];
        uu[n]=D[0][j+1]/(double)TD5_TG_SPAN_LENGTH; vv[n]=U[0]; n++;
        px[n]=X[1][j+1]; py[n]=Y[1][j+1]; pz[n]=Z[1][j+1];
        uu[n]=D[1][j+1]/(double)TD5_TG_SPAN_LENGTH; vv[n]=U[1]; n++;
        px[n]=X[1][j];   py[n]=Y[1][j];   pz[n]=Z[1][j];
        uu[n]=D[1][j]  /(double)TD5_TG_SPAN_LENGTH; vv[n]=U[1]; n++;
    }
    /* [R9 TOPO C4] The apron is the same SURFACE the skirt it seams to is, so
     * it reads the same ground-run material. It used to read the far-group
     * owner's DITHERED biome, which at a boundary could put a tile apron behind
     * a grass skirt (or the reverse) at the very seam they share. */
    seg_page[0] = tg_topo_surface_page(h->si);
    seg_nq[0]   = 3;

    if (ridge_ok && tg_terrain_ridge_enabled()) {
        /* Ridge: a wall standing on the outermost edge, its top sampled from the
         * same hill function so consecutive groups share a crest height and the
         * skyline is one continuous ridge line. Snow biomes get a white flank
         * for the same reason the skirt does. */
        double t0 = TD5_TG_RIDGE_BASE + tg_terrain_hill_y(X[0][3], Z[0][3], 3600.0);
        double t1 = TD5_TG_RIDGE_BASE + tg_terrain_hill_y(X[1][3], Z[1][3], 3600.0);
        if (t0 < TD5_TG_RIDGE_MIN_UP) t0 = TD5_TG_RIDGE_MIN_UP;
        if (t1 < TD5_TG_RIDGE_MIN_UP) t1 = TD5_TG_RIDGE_MIN_UP;
        /* The wall's height is measured from its own base, and that base is now
         * on the global floor rather than under the emitting span. On a stretch
         * of road high above the floor a purely relative crest therefore falls
         * BELOW the horizon and the skyline opens up. Require it to clear the
         * road it is seen from instead, which is what a ridge does. */
        if (Y[0][3] + t0 < B[0] + TD5_TG_RIDGE_MIN_UP)
            t0 = B[0] + TD5_TG_RIDGE_MIN_UP - Y[0][3];
        if (Y[1][3] + t1 < B[1] + TD5_TG_RIDGE_MIN_UP)
            t1 = B[1] + TD5_TG_RIDGE_MIN_UP - Y[1][3];
        /* [R4 item 5] Half-texel V inset. The band maps v=0 at the crest and
         * v=1 at the base; reaching EXACTLY 0.0/1.0 samples the page's edge texel
         * row, and under the wrapped sampler the top edge (v=0) fetches the
         * bottom row (v=1, the darkest foliage base) -- the reported "black line
         * at the top of the texture". e0..e1 keeps both edges off the border.
         * flag off restores the raw 0/1 for the A/B. */
        const int    tfix = tg_r4_treeline_fix();
        const double vt = tfix ? TD5_TG_FACADE_UV_INSET       : 0.0;
        const double vb = tfix ? 1.0 - TD5_TG_FACADE_UV_INSET : 1.0;
        /* [R5 item 16] Horizontal tiling. The ridge quad spans one far-group, but
         * its two ends are at TD5_TG_FAR_REACH (30000) OUT, so on a curve they fan
         * far wider than the FAR_GROUP span-index delta the old U used (a fixed 4
         * tiles). That stretched each 64-texel canopy tile to thousands of world
         * units -- the reported "trees texture looks very stretched out". Tile by
         * the ridge's ACTUAL world width instead, one tile per span-length, so a
         * tile is the same size on a straight and through a bend. The page wraps
         * and every quad starts at U=0 and ends on an integer tile count, so the
         * join to the next group's quad stays continuous. */
        double u_near = U[0], u_far = U[1];
        /* [R15 BAND item 3] The urban skyline takes the SAME world-width tiling.
         * Mirrors the seg_page[1] routing below (snow -> flank, urban -> skyline,
         * else tree line) so the branch that gets the skyline page is exactly the
         * branch that gets the skyline's U. */
        const int r5skyline = !tg_biome_is_snow(h->b) && tg_r4_city_skyline()
                            && h->b->urbanity >= 2 && tg_r15_skyline_uv();
        if (r5fix && (r5treeline || r5skyline)) {
            const double w = sqrt((X[1][3]-X[0][3])*(X[1][3]-X[0][3])
                                + (Z[1][3]-Z[0][3])*(Z[1][3]-Z[0][3]));
            /* [R8 item 14 ASPECT] Tile width = the wall's own crest height, so
             * a square page is drawn square. Was TD5_TG_SPAN_LENGTH (1500)
             * against a ~4500 wall: a 1:3 vertical stretch of a 64x64 page. */
            /* [R15 BAND item 3] The SKYLINE takes the constant-world-width
             * branch, NOT the crest-height one. MEASURED on seed 1459285111:
             * the urban ridge averages 8221 wide by 28359 tall per quad, so
             * "tile width = crest height" gives floor(8221/28359 + 0.5) = 0,
             * clamped to ONE tile -- 4x WIDER than the fixed 4 tiles it
             * replaced, i.e. more stretch, the opposite of the report. That
             * rule is right for a tree line (roughly as wide as it is tall) and
             * wrong for a ridge four times taller than its own quad. Tiling by
             * span length keeps a tile the same world size through a bend --
             * which is what R5 item 16 was actually for -- and gives ~5 tiles
             * here instead of a fixed 4. */
            const double tw = (r5skyline && !r5treeline)
                            ? (double)TD5_TG_SPAN_LENGTH
                            : (tg_r8_treeline_aspect()
                               ? (0.5 * (t0 + t1)) : (double)TD5_TG_SPAN_LENGTH);
            double tiles = floor(w / (tw > 1.0 ? tw : 1.0) + 0.5);
            if (tiles < 1.0) tiles = 1.0;
            u_near = 0.0; u_far = tiles;
            /* [R8 item 14 VARY] Integer per-group phase: shifts which texel
             * columns a group starts on without breaking the wrap. */
            if (tg_r8_treeline_vary()) {
                const unsigned hh = (unsigned)(g0 / TD5_TG_FAR_GROUP)
                                  * 2246822519u;
                const double ph = (double)((hh >> 17) % 7u);
                u_near += ph; u_far += ph;
            }
        }
        /* near-bottom, far-bottom, far-top, near-top; v = 1 at the base, so the
         * page's top rows (v = 0) land on the crest. */
        px[n]=X[0][3]; py[n]=Y[0][3];      pz[n]=Z[0][3]; uu[n]=u_near; vv[n]=vb; n++;
        px[n]=X[1][3]; py[n]=Y[1][3];      pz[n]=Z[1][3]; uu[n]=u_far;  vv[n]=vb; n++;
        px[n]=X[1][3]; py[n]=Y[1][3] + t1; pz[n]=Z[1][3]; uu[n]=u_far;  vv[n]=vt; n++;
        px[n]=X[0][3]; py[n]=Y[0][3] + t0; pz[n]=Z[0][3]; uu[n]=u_near; vv[n]=vt; n++;
        /* [R3 item 8] This distant ridge IS the "background tree line" the user
         * reported as "grey at the bottom and white at the top ... doesn't seem
         * to be a tree texture there". It was drawn on TD5_TG_PAGE_HILL, whose
         * palette is literally grey rock (idx 0..9) under a white snowline (idx
         * 10..15) -- correct for a snowy peak, wrong everywhere else, and NOT a
         * tree. (The r2 attempt rebuilt the flora TREELINE band instead, which
         * never even emits on a seed with no FOREST/ALPINE/FIELDS biome -- e.g.
         * 99991 -- so it could not have fixed what is on screen.) Draw the ridge
         * on the green alpha-keyed canopy page in every non-snow biome: its
         * ragged keyed top reads as a forested skyline silhouette against the
         * sky, which is exactly the tree line the user expected. Snow/alpine keep
         * the snowy flank. TD5_TG_PAGE_HILL stays the tunnel rock massing page,
         * untouched. */
        /* [R4 item 5] "which ones are suitable for cities": a forest canopy
         * behind a city skyline reads wrong. Snow keeps its white flank; the
         * urban biomes (urbanity >= 2: CITY, INDUSTRIAL, ORIENTAL) get the blocky
         * building-tops skyline page; everything else keeps the tree line. Gated
         * so the A/B toggles ONE thing. */
        if (tg_biome_is_snow(h->b))
            seg_page[1] = tg_ground_page_for_span(h->si, h->b);
        else if (tg_r4_city_skyline() && h->b->urbanity >= 2)
            seg_page[1] = TD5_TG_PAGE_R4_SKYLINE;
        else
            seg_page[1] = tg_r8_treeline_page(g0);   /* [R8 item 14 VARY] */
        if (seg_page[1] >= TD5_TG_PAGE_R8_TREELINE &&
            seg_page[1] <  TD5_TG_PAGE_R8_TREELINE + TD5_TG_R8_TREELINE_N)
            tg_acct(TG_ACCT_R8_TERRAIN, h->si);
        /* [R4 seam] account the urban-skyline ridge under the FLOW bucket so the
         * inventory reflects the reclassification (renamed in place). */
        if (seg_page[1] == TD5_TG_PAGE_R4_SKYLINE)
            tg_acct(TG_ACCT_R4_FLOW, h->si);
        /* [R8 item 14] Record the world size one page tile is drawn at, on the
         * tree-line ridges only -- the ones the report is about. */
        if (seg_page[1] != TD5_TG_PAGE_R4_SKYLINE && !tg_biome_is_snow(h->b)) {
            const double w = sqrt((X[1][3]-X[0][3])*(X[1][3]-X[0][3])
                                + (Z[1][3]-Z[0][3])*(Z[1][3]-Z[0][3]));
            const double nt = (u_far - u_near);
            if (nt > 0.0) tg_r8_tl_note(w / nt, 0.5 * (t0 + t1), seg_page[1]);
        }
        /* [R15 BAND item 3] Same measurement for the skyline branch, which the
         * line above deliberately skips. Before this round nt was a fixed 4
         * tiles across a quad fanned out to TD5_TG_FAR_REACH, so this reports
         * the stretch the user saw and, with the knob on, its correction. */
        else if (seg_page[1] == TD5_TG_PAGE_R4_SKYLINE) {
            const double w = sqrt((X[1][3]-X[0][3])*(X[1][3]-X[0][3])
                                + (Z[1][3]-Z[0][3])*(Z[1][3]-Z[0][3]));
            const double nt = (u_far - u_near);
            if (nt > 0.0) tg_r15_sky_note(w / nt, 0.5 * (t0 + t1));
        }
        seg_nq[1]   = 1;
        nseg = 2;
    }

    h->moff[(*h->nmesh)] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg))
        return 0;
    (*h->nmesh)++;
    /* [R13 BAND item 1b] Byte attribution: what a far band actually costs, and
     * (report-only runs) what the hidden ones cost. The hidden accumulator was
     * charged the pre-write length above, so this closes it. */
    {
        const long bytes = (long)h->blk->len - (long)h->moff[*h->nmesh - 1];
        s_r13_far_meshes++;
        s_r13_far_bytes += bytes;
        if (r13_hidden) s_r13_far_hidden_bytes += bytes;
    }
    /* One band covers the whole far-group, not just its owner span. */
    tg_acct_range(TG_ACCT_FARBAND, g0, g1);
    return 1;
}

/* [R5 item 14] Does the far-group owned by span si sit over a bridge run?
 * The far band is emitted per 4-span group (TD5_TG_FAR_GROUP); a bridge run is
 * 40 spans and 40-aligned, so a whole run maps onto ten complete groups and no
 * group straddles a run boundary. Test both group ends anyway so a future
 * unaligned run still reads correctly. */
static int tg_far_group_over_bridge(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    return tg_span_in_bridge_run(g0) || tg_span_in_bridge_run(g1);
}

static int tg_far_group_near_tunnel(int si)
{
    const int m = td5_env_int("TD5RE_AUTOTRACK_TUNNEL_FARCLEAR",
                              TD5_TG_TUNNEL_FARCLEAR, 0, 400);
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    int s;
    for (s = g0 - m; s <= g1 + m; s++)
        if (s >= 0 && tg_span_in_tunnel(s)) return 1;
    return 0;
}

static int tg_far_group_over_fork(int si)
{
    const int g0 = (si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    const int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    int i;
    if (!tg_branches_enabled()) return 0;
    for (i = 0; i < s_fork_count; i++) {
        const int a = s_forks[i].F - TD5_TG_BRANCH_WIDEN - 2;
        const int b = s_forks[i].R + TD5_TG_FAR_FORK_PAD;
        if (g1 >= a && g0 <= b) return 1;   /* group overlaps the fork + bow */
    }
    return 0;
}

static int tg_emit_far_shore(const TG_FBHook *h, int is_left)
{
    const TG_NodeList *nl = h->nl;
    const int g0 = (h->si / TD5_TG_FAR_GROUP) * TD5_TG_FAR_GROUP;
    int g1 = g0 + TD5_TG_FAR_GROUP - 1;
    /* [R9 BRIDGE item 10] "On span 1039 there's buildings in the background
     * that are over the water."
     *
     * MEASURED (R9 over-water audit, seed 99991): this wall is the offender, and
     * the reason it is a BUILDING one is its own page routing five lines below
     * -- on an urban biome it draws TD5_TG_PAGE_R4_SKYLINE, a city silhouette.
     * R8 stood it TD5_TG_SHORE_FAR_INSET *inside* the water plane's outer edge,
     * so a row of towers rose straight out of open sea. A far shore is LAND: put
     * it just OUTSIDE the plane's edge instead, where the water ends, and it
     * closes the same horizon while standing on the far bank rather than in the
     * bay. One sign, and it is the difference between a skyline and a mirage.
     * TD5RE_R9_BRIDGE_FARSHORE=0 restores the R8 inset for an A/B. */
    /* [R17 TERRAIN item 1] "this forest is at the edge of the screen near the
     * water." The far-shore treeline stood INSIDE the sea plane. `out` is applied
     * from the ROAD EDGE (see ex[e] below), but the sea plane runs from its
     * shoreline gap TD5_TG_WATER_BEACH (8100) out to TD5_TG_WATER_BEACH +
     * TD5_TG_WATER_EXTENT (58100) from that same edge -- see tg_emit_water /
     * tg_r11_sea_outer. The R9 placement used WATER_EXTENT +/- INSET, i.e. it
     * treated WATER_EXTENT as the outer-edge distance and dropped the BEACH term,
     * so even the "outside" branch (54000) fell ~4100 units SHORT of the real
     * outer edge and the treeline rose out of open sea. Anchor it a fixed INSET
     * BEYOND the sea plane's true outer edge so it stands on the far bank.
     * TD5RE_R17_FARSHORE_OUTSIDE=0 restores the R9 EXTENT-relative value. */
    const double out = td5_env_flag_on("TD5RE_R17_FARSHORE_OUTSIDE")
                     ? (double)TD5_TG_WATER_BEACH + (double)TD5_TG_WATER_EXTENT
                       + TD5_TG_SHORE_FAR_INSET
                     : (td5_env_flag_on("TD5RE_R9_BRIDGE_FARSHORE")
                        ? (double)TD5_TG_WATER_EXTENT + TD5_TG_SHORE_FAR_INSET
                        : (double)TD5_TG_WATER_EXTENT - TD5_TG_SHORE_FAR_INSET);
    double px[4], py[4], pz[4], uu[4], vv[4];
    double ex[2], ey[2], ez[2];
    int seg_page, seg_nq = 1, e;

    if (!td5_env_flag_on("TD5RE_R8_TERRAIN_SHORE")) return 1;
    if (g1 > nl->count - 2) g1 = nl->count - 2;
    if (g1 < g0) return 1;
    if (*h->nmesh + 1 >= h->maxmesh) return 1;

    for (e = 0; e < 2; e++) {
        const int se = e ? g1 : g0;
        double lx, ly, lz, rx, ry, rz, ux, uz, len;

        tg_road_edge(nl, se, e ? 1.0 : 0.0, 0.0, 1.0,
                     &lx, &ly, &lz, &rx, &ry, &rz);
        ux = lx - rx; uz = lz - rz;
        len = sqrt(ux * ux + uz * uz);
        if (len < 1e-6) { ux = 1.0; uz = 0.0; } else { ux /= len; uz /= len; }
        if (!is_left) { ux = -ux; uz = -uz; }
        ex[e] = (is_left ? lx : rx) + ux * out;
        ez[e] = (is_left ? lz : rz) + uz * out;
        ey[e] = tg_sea_level_y(nl, se);
    }

    /* Tile by the shore's own world width, one tile per crest height, so the
     * page keeps its authored 1:1 aspect (same rule as the R8 ridge below). */
    {
        const double w = sqrt((ex[1]-ex[0])*(ex[1]-ex[0])
                            + (ez[1]-ez[0])*(ez[1]-ez[0]));
        double tiles = floor(w / TD5_TG_SHORE_FAR_HIGH + 0.5);
        if (tiles < 1.0) tiles = 1.0;
        /* near-bottom, far-bottom, far-top, near-top; v = 1 at the waterline. */
        px[0]=ex[0]; py[0]=ey[0];                        pz[0]=ez[0];
        px[1]=ex[1]; py[1]=ey[1];                        pz[1]=ez[1];
        px[2]=ex[1]; py[2]=ey[1]+TD5_TG_SHORE_FAR_HIGH;  pz[2]=ez[1];
        px[3]=ex[0]; py[3]=ey[0]+TD5_TG_SHORE_FAR_HIGH;  pz[3]=ez[0];
        uu[0]=0.0;   vv[0]=1.0 - TD5_TG_FACADE_UV_INSET;
        uu[1]=tiles; vv[1]=1.0 - TD5_TG_FACADE_UV_INSET;
        uu[2]=tiles; vv[2]=TD5_TG_FACADE_UV_INSET;
        uu[3]=0.0;   vv[3]=TD5_TG_FACADE_UV_INSET;
    }

    seg_page = tg_biome_is_snow(h->b) ? tg_ground_page_for_span(h->si, h->b)
             : (h->b->urbanity >= 2  ? TD5_TG_PAGE_R4_SKYLINE
                                     : tg_r8_treeline_page(g0));

    h->moff[(*h->nmesh)] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
        return 0;
    (*h->nmesh)++;
    tg_acct_range(TG_ACCT_R8_TERRAIN, g0, g1);
    return 1;
}

int tg_emit_fb_terrain(const TG_FBHook *h)
{
    double wsd;
    int s, ridge_ok = 1, ridge_gate;

    if (!tg_terrain_far_enabled()) return 1;
    if (!tg_far_group_owner(h->si)) return 1;

    /* [R5 item 14] Leave ONLY water on the floor below a bridge. The distant
     * terrain apron + ridge/skyline wall (tg_emit_far_band) is not gated on the
     * crossing, so over every bridge run it draped a grass/concrete apron and a
     * building-tops skyline down into the gorge -- the "still plenty of road,
     * building backgrounds mixed with grass" report. Suppress it over a bridge
     * group: the bridge water plane (BRIDGE_WATER_HALF 32000) already reaches
     * past the far band's own reach (FAR_REACH 30000), so removing the band
     * leaves open water to the horizon with nothing to cover, and the sloped
     * shore is carried by the gorge banks (tg_ground_side gorge branch) and the
     * longitudinal coastline strips (tg_emit_bridge_coast). The suppression
     * shows in the element inventory as a far-bands GAP over each run.
     * TD5RE_AUTOTRACK_BRIDGE_CLEARFAR=0 restores the draped band for an A/B. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_CLEARFAR") &&
        tg_far_group_over_bridge(h->si))
        return 1;

    /* [R8 TERRAIN items 5/15] THE TUNNEL AND FORK GATES ARE RIDGE GATES, NOT
     * GROUND GATES -- and until round 8 both dropped the whole band.
     *
     * "on the right side of span 569 there's just a few meters of terrain
     * outside road" (99991) and "on the left side there's barely any geometry"
     * (777 @200) are one complaint: the world ends too close to the road. R7
     * answered a third sighting of it with verge PROPS and the user came back
     * saying the FIELD is still narrow, so the defect is the ground EXTENT.
     *
     * MEASURED (tg_r8_terrain_extent_report, both seeds, before this change):
     *   99991: 1289 of 3972 span-sides (32.4%) end inside 20000 units.
     *          At span 569 the RIGHT side reaches 12000 and the LEFT 30000 --
     *          same skirt on both sides, so R6 CITY's 24000->12000 verge
     *          narrowing is NOT what the user is hitting. The asymmetry is
     *          entirely the fork gate. Blocked sides: fork 220, tunnel 600,
     *          bridge 372, sea 185.
     *   777:   1678 of 3972 (42.2%); ALPINE is the worst biome at mean 15030
     *          with 496 of its 600 sides narrow. Blocked: tunnel 840,
     *          bridge 692, sea 216, fork 106.
     *
     * Read the two gates' own recorded reasons and they are both about the
     * standing WALL, never the ground under it:
     *   - fork (R6 item 4): "the right far-band SKYLINE ... lands ON the branch
     *     carriageway as a row of grey slabs crossing the street".
     *   - tunnel (R6 TUNNEL item 8a): "the distant RIDGE standing beyond that
     *     mouth reads as a grey slab down the road".
     * The apron is flat, sits at or below road level and falls away outward, so
     * it can be neither a slab across a street nor a slab down a bore. Suppress
     * the ridge on those two and keep the ground.
     *
     * The BRIDGE and SEA gates are different in kind and stay whole-band: R5
     * item 14's report was that the band "draped a grass/concrete APRON ... down
     * into the gorge", i.e. the ground itself was wrong there, and on the seaward
     * side the ground is water. Those two are the gates whose reason survives.
     *
     * TD5RE_R8_TERRAIN_EXTENT=0 restores the round-7 whole-band suppression. */
    ridge_gate = td5_env_flag_on("TD5RE_R8_TERRAIN_EXTENT");

    if (td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_CLEARFAR") &&
        tg_far_group_near_tunnel(h->si)) {
        if (!ridge_gate) return 1;
        ridge_ok = 0;
    }

    /* Seaward side is the sea's, not the plain's -- the water plane already
     * reaches 50000 out there and a grass band would float over it. */
    wsd = h->b->water ? tg_water_side(h->si) : 0.0;

    for (s = 0; s < 2; s++) {
        const int is_left = s ? 1 : 0;
        int side_ridge = ridge_ok;
        if ((wsd > 0.0 && is_left) || (wsd < 0.0 && !is_left)) {
            /* [R8 TERRAIN item 15] The seaward side is the one the user called
             * "barely any geometry": measured, span 200 on 777 reaches 9600 on
             * the left (the shore ramp) against 30000 on the right, and past
             * that there is only the flat water plane out to 50000. Correct
             * ground, empty horizon. Close it with a far SHORE instead of a
             * grass band -- one wall standing on the sea surface at the water
             * plane's own outer edge, so the ocean ends in a coastline rather
             * than in nothing. */
            if (!TG_SUB2(TG_SUB2_FARSHORE, tg_emit_far_shore(h, is_left))) return 0;
            continue;
        }
        /* [R6 item 4] The RIGHT band's ridge over a fork lands on the branch;
         * its ground does not. Drop only the ridge (see the note above). */
        if (!is_left && td5_env_flag_on("TD5RE_AUTOTRACK_FORK_CLEARFAR") &&
            tg_far_group_over_fork(h->si)) {
            if (!ridge_gate) continue;
            side_ridge = 0;
        }
        if (*h->nmesh + 2 >= h->maxmesh) break;
        if (!TG_SUB2(TG_SUB2_FARBAND, tg_emit_far_band(h, is_left, side_ridge))) return 0;
        if (side_ridge != ridge_ok || !side_ridge)
            tg_acct(TG_ACCT_R8_TERRAIN, h->si);
    }
    return 1;
}

/* [R13 BAND] MODELS.DAT total, recorded at the write so the band report can
 * state its saving as a SHARE rather than as a bare byte count. */
long s_r13_models_bytes;

/* [R13 BAND] The class-level inventory both items are argued from. Always
 * printed (this is the round's evidence, not an opt-in trace); the per-group
 * detail lines are behind TD5RE_R13_BAND_REPORT=1. */
void tg_r13_band_report(const TG_NodeList *nl, int nspans)
{
    int si, walled = 0, grid_walled = 0;

    for (si = 0; si < nspans && si + 1 < nl->count; si++) {
        double t, l;
        int sides = (tg_r13_band_side(nl, si,  1.0, &t, &l) ? 1 : 0)
                  + (tg_r13_band_side(nl, si, -1.0, &t, &l) ? 1 : 0);
        if (sides) {
            walled++;
            if (si <= TD5_TG_GRID_SPAN) grid_walled++;
        }
    }
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R13 BAND 1a] tree-line spans=%d of %d, of which "
              "grid spans 0..%d walled=%d (banner sits on span %d) "
              "(knob TD5RE_R13_BAND_GRID=%s)",
              walled, nspans, TD5_TG_GRID_SPAN, grid_walled, TD5_TG_GRID_SPAN,
              tg_r13_band_grid() ? "on" : "off");
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R13 BAND 1b] far bands written=%ld (%ld bytes); "
              "behind the wall: whole=%ld (%ld bytes) ridge-only=%ld; "
              "skyline kept=%ld; far-band share of MODELS.DAT=%.2f%%, "
              "hidden share=%.2f%% (knob TD5RE_R13_BAND_CULL=%s)",
              s_r13_far_meshes, s_r13_far_bytes,
              s_r13_far_hidden, s_r13_far_hidden_bytes, s_r13_far_ridge,
              s_r13_far_skyline_kept,
              s_r13_models_bytes > 0
                ? 100.0 * (double)s_r13_far_bytes
                        / (double)s_r13_models_bytes : 0.0,
              s_r13_models_bytes > 0
                ? 100.0 * (double)s_r13_far_hidden_bytes
                        / (double)s_r13_models_bytes : 0.0,
              tg_r13_band_cull() ? "on" : "off");
}

/* [R14 COAST item 5b] "in the background the forest ends abruptly -- the tree
 * line should wrap around the coastline."
 *
 * The ledger above counts how MANY spans are walled. This one reports WHERE each
 * wall run ENDS and WHY, per side, with the height the wall still had on its
 * last span. A run that ends on a full-height face is the "abrupt end"; a run
 * that ends at 0 has tapered. Always printed while a run ends tall, because that
 * is the defect this area is measuring. */
void tg_r14_band_report(const TG_NodeList *nl, int nspans)
{
    int s, side_i, abrupt = 0, tapered = 0, logged = 0, wrapped = 0;

    for (side_i = 0; side_i < 2; side_i++) {
        const double side = side_i ? 1.0 : -1.0;
        int prev = 0;
        for (s = 0; s < nspans && s + 1 < nl->count; s++) {
            double t, l, h = 0.0, back = 0.0;
            const int now = tg_r13_band_side(nl, s, side, &t, &l);
            if (prev && !now) {
                const int e = s - 1;               /* last walled span */
                const int cell = tg_biome_cell_index(e);
                const int nxt = tg_biome_cell_index(s);
                tg_r12_band_params(e, &h, &back);
                if (h > 1000.0) abrupt++; else tapered++;
                /* A tall end whose cut is a WATER BRIDGE RUN is the class item
                 * 5b reports, and it is the class the wrap closes. Counted
                 * separately because the wrap deliberately does not change
                 * PRESENCE, so `abrupt` cannot move even when it is fixed. */
                if (h > 1000.0 && tg_span_in_bridge_run(s) &&
                    tg_water_span_clear(s)) wrapped++;
                if (logged < 24) {
                    logged++;
                    TD5_LOG_W(LOG_TAG,
                              "R14BAND end: %s side, run ends at span %d "
                              "h=%.0f back=%.0f biome %s -> %s next-span "
                              "bridge=%d water=%d",
                              side > 0.0 ? "LEFT" : "RIGHT", e, h, back,
                              k_biomes[cell].name, k_biomes[nxt].name,
                              tg_span_in_bridge_run(s) ? 1 : 0,
                              tg_biome_span_has_water(s) ? 1 : 0);
                }
            }
            prev = now;
        }
    }
    TD5_LOG_I(LOG_TAG, "R14BAND: wall runs ending TALL (h>1000)=%d, of which "
              "cut by a WATER BRIDGE RUN=%d; ending tapered=%d; "
              "wraps emitted=%ld (mean reach %.0f) "
              "(knob TD5RE_R14_COAST_WRAP=%s)",
              abrupt, wrapped, tapered, s_r14_wraps,
              s_r14_wraps ? s_r14_wrap_reach / (double)s_r14_wraps : 0.0,
              td5_env_flag_on("TD5RE_R14_COAST_WRAP") ? "on" : "off");
}

/* ===================== [R8 TERRAIN items 5/15] EXTENT DIAGNOSTIC =====================
 * "on the right side of span 569 there's just a few meters of terrain outside
 * road, it has to be a much bigger field" (99991) and "on the left side of the
 * track there's barely any geometry" (777 @200) are the SAME complaint: the
 * world ends too close to the road. R7's BRANCH item 10 answered a third
 * sighting of it with verge PROPS, and the user came back saying the FIELD is
 * still only a few meters wide -- so the defect is the ground EXTENT, not what
 * stands on it.
 *
 * Three rounds of this item have been argued from frames. This reports the
 * arithmetic instead: for every span and both sides, the skirt's outer reach,
 * whether the far band covers that side, and if not WHICH gate suppressed it.
 * "Effective extent" is what the user can actually see ground on.
 * TD5RE_R8_TERRAIN_EXTENT_LOG=1. */
/* Mirrors tg_emit_fb_terrain's gate chain exactly, including the R8 ridge-only
 * split, so before/after is measured on one model and not on two. Returns the
 * reason the GROUND is absent, or NULL; *ridge is cleared where the band emits
 * ground but no standing wall. */
static const char k_r8_why_fork[]   = "fork";

static const char k_r8_why_bridge[] = "bridge";

static const char k_r8_why_tunnel[] = "tunnel";

static const char k_r8_why_sea[]    = "sea";

static const char k_r8_why_off[]    = "far-off";

static const char *tg_r8_far_block_reason(const TG_NodeList *nl, int si,
                                          const TG_Biome *b, int is_left,
                                          int *ridge)
{
    const int rg = td5_env_flag_on("TD5RE_R8_TERRAIN_EXTENT");
    double wsd;
    (void)nl;
    *ridge = 1;
    if (!tg_terrain_far_enabled())                       return k_r8_why_off;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGE_CLEARFAR") &&
        tg_far_group_over_bridge(si))                    return k_r8_why_bridge;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_TUNNEL_CLEARFAR") &&
        tg_far_group_near_tunnel(si)) {
        if (!rg) return k_r8_why_tunnel;
        *ridge = 0;
    }
    wsd = b->water ? tg_water_side(si) : 0.0;
    if ((wsd > 0.0 && is_left) || (wsd < 0.0 && !is_left)) return k_r8_why_sea;
    if (!is_left && td5_env_flag_on("TD5RE_AUTOTRACK_FORK_CLEARFAR") &&
        tg_far_group_over_fork(si)) {
        if (!rg) return k_r8_why_fork;
        *ridge = 0;
    }
    return NULL;
}

/* ==================================================================
 * SECTION: [R9 TOPO] THE CHAIN -- one span-side's whole ground surface
 *
 * The clauses are stated at the head of the file (SECTION: TOPOGRAPHIC
 * CONTINUITY AUTHORITY). This is where they are MEASURED and where the flora
 * consumer reads the surface from, and it lives here rather than up there
 * because the chain's outer half is the far band and needs its constants.
 * ================================================================== */

/* Does this span-side's far band emit GROUND, and does a wall stand on it?
 * Mirrors tg_emit_fb_terrain's gate chain via the R8 reason helper, so the
 * chain and the emitter cannot disagree about what exists. */
static int tg_topo_far_state(const TG_NodeList *nl, int si,
                             const TG_Biome *b, int is_left, int *closed)
{
    int ridge = 1;
    const char *why;
    *closed = 0;
    if (!tg_far_group_owner(si) && 0) return 0;   /* bands cover their group */
    why = tg_r8_far_block_reason(nl, si, b, is_left, &ridge);
    if (why) {
        /* The sea IS a closed horizon (R8's far shore stands on it); a bridge
         * gorge is closed by its own banks and water. Neither is an open edge. */
        *closed = (why == k_r8_why_sea || why == k_r8_why_bridge);
        return 0;
    }
    *closed = ridge && tg_terrain_ridge_enabled();
    return 1;
}

/* Build the whole chain for one span-side: skirt points, then the far band's
 * four rings where it emits. `dy` is DROP BELOW THE ROAD EDGE, positive down,
 * the same sign convention TG_GroundProf uses, so the two halves concatenate. */
void tg_topo_chain(const TG_NodeList *nl, int si, int is_left,
                          TG_TopoChain *c)
{
    const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
    const double wsd = b->water ? tg_water_side(si) : 0.0;
    const double base_reach = tg_far_reach();
    const int sink = td5_env_flag_on("TD5RE_AUTOTRACK_TERRAIN_SINK");
    const double floor_drop = nl->v[si].y
                            - (tg_track_min_y(nl) - TD5_TG_FAR_SINK_AT);
    TG_GroundProf p;
    double so, seam, reach, cap;
    int k, has_far;

    memset(c, 0, sizeof(*c));
    tg_ground_side(nl, si, is_left, wsd, &p);
    for (k = 0; k < p.n && c->n < TD5_TG_TOPO_MAXPT; k++) {
        c->d[c->n]  = p.d[k];
        c->dy[c->n] = p.dy[k];
        c->n++;
    }
    so   = p.d[p.n - 1];
    seam = p.dy[p.n - 1] + TD5_TG_FAR_SINK;

    has_far = tg_topo_far_state(nl, si, b, is_left, &c->closed);
    if (has_far) {
        static const double k_ring[4] = { 0.0, 0.18, 0.45, 1.0 };
        reach = base_reach;
        cap   = tg_topo_road_cap(nl, si, is_left);
        if (!c->closed && sink)
            reach = tg_topo_runout_reach(so, seam < floor_drop
                                             ? floor_drop - seam : 0.0,
                                         base_reach);
        if (cap < reach) reach = cap;
        if (reach < so + TD5_TG_FAR_TUCK + 500.0)
            reach = so + TD5_TG_FAR_TUCK + 500.0;
        for (k = 1; k < 4 && c->n < TD5_TG_TOPO_MAXPT; k++) {
            const double dd = so + (reach - so) * k_ring[k];
            double dy = sink ? seam + (floor_drop - seam) * k_tg_far_sink[k]
                             : seam;
            if (dy < seam) dy = seam;
            c->d[c->n]  = dd;
            c->dy[c->n] = dy;
            c->n++;
        }
    }

    /* C1: the chain must start at the road edge and never jump laterally.
     * d[0] > 0 IS legal on the right of a fork -- the branch carriageway and
     * the gore floor occupy that strip -- so the start is only a gap where no
     * carriageway explains it. */
    c->gap = 0.0;
    if (c->d[0] > TD5_TG_TOPO_GAP_TOL && !tg_span_in_bridge_run(si)) {
        /* A branch carriageway and the gore floor legitimately occupy the strip
         * the right-hand skirt stands off. A GORGE inset is legitimate too --
         * the deck is elevated and there is deliberately no ground under it --
         * which is why bridge runs are excluded outright rather than netted off:
         * on this seed they account for 304 of the pre-fix "gaps" and every one
         * of them is correct, so counting them would drown the real holes. */
        const double explained = tg_ground_branch_clear(nl, si);
        const double g = c->d[0] - explained;
        if (g > c->gap) c->gap = g;
    }
    /* The far band tucks UNDER the skirt by TD5_TG_FAR_TUCK, so its first ring
     * is inboard of the skirt's outer point by construction; a positive step
     * anywhere in the chain would be a hole. */
    for (k = 1; k < c->n; k++) {
        const double g = c->d[k] - c->d[k - 1];
        if (g < 0.0 && -g > c->gap) c->gap = -g;
    }

    c->horiz = c->d[c->n - 1];
    c->drop  = c->dy[c->n - 1];
    c->surf  = 0.0;
    for (k = 1; k < c->n; k++) {
        const double dd = c->d[k] - c->d[k - 1];
        const double dv = c->dy[k] - c->dy[k - 1];
        c->surf += sqrt(dd * dd + dv * dv);
    }
    c->end_grade = 0.0;
    if (c->n >= 2) {
        const double dd = c->d[c->n - 1] - c->d[c->n - 2];
        if (dd > 1e-6) c->end_grade = (c->dy[c->n - 1] - c->dy[c->n - 2]) / dd;
    }
    c->road_cap = tg_topo_road_cap(nl, si, is_left);
}

/* Drop below the road edge at outward distance d, from the WHOLE chain --
 * skirt and far band. This is what "the trees are not following" needs:
 * tg_flora_plant clamps to the skirt's last point, so anything planted past
 * the skirt stands at the LIP height while the ground under it has descended.
 * Linear between chain points, flat past the end. */
double tg_topo_drop_at(const TG_TopoChain *c, double d)
{
    int k;
    if (c->n <= 0) return 0.0;
    if (d <= c->d[0]) return c->dy[0];
    for (k = 0; k + 1 < c->n; k++) {
        if (d <= c->d[k + 1]) {
            const double w = c->d[k + 1] - c->d[k];
            const double t = (w > 1e-6) ? (d - c->d[k]) / w : 0.0;
            return c->dy[k] + t * (c->dy[k + 1] - c->dy[k]);
        }
    }
    return c->dy[c->n - 1];
}

int tg_emit_fb_slope_flora(const TG_FBHook *h)
{
    const TG_NodeList *nl = h->nl;
    const int si = h->si;
    /* [R17 TERRAIN item 4] Same continuity fix as tg_emit_fb_park_trees: take the
     * billboard/species biome from the R11 hard-edge index so this slope planting
     * does not blink out on the facade-dithered spans of a paved<->tree blend
     * band. TD5RE_R17_FLORA_HARDEDGE=0 restores the dithered per-span biome. */
    const TG_Biome *b = td5_env_flag_on("TD5RE_R17_FLORA_HARDEDGE")
                      ? &k_biomes[tg_scenery_biome_index(si)] : h->b;
    TG_TopoChain c;
    unsigned int hh;
    double side, d, tw, th, jit, cx, cz, lx, lz, drop_in, base_y;
    int s, v, page;

    if (!tg_topo_enabled()) return 1;
    if (!td5_env_flag_on("TD5RE_R9_TOPO_FLORA")) return 1;
    if (!tg_real_textures_enabled())     return 1;
    if (k_r5_flora_tree_count <= 0)      return 1;
    if (!b->billboard || b->tree_n <= 0) return 1;
    if (si <= TD5_TG_GRID_SPAN)          return 1;
    if (si + 1 >= nl->count)             return 1;
    if (tg_span_in_bridge_run(si))       return 1;   /* the gorge is water */
    if (tg_span_in_tunnel(si))           return 1;
    if (*h->nmesh + 1 >= h->maxmesh)     return 1;

    hh = (unsigned)si * 2654435761u + 0x9E3779B9u;
    if ((int)(hh >> 29) > (b->density >> 1)) return 1;   /* sparse rank */
    s    = (int)((hh >> 4) & 1u);
    side = s ? 1.0 : -1.0;
    if (tg_side_blocked(si, side)) return 1;
    if (b->water && side == tg_water_side(si)) return 1; /* never over the sea */

    tg_topo_chain(nl, si, side > 0.0, &c);
    if (c.n < 2) return 1;
    /* Only a FALLING side gets a slope rank; on flat ground the near planters
     * already cover what you can see and a distant billboard is just cost. */
    if (c.drop - c.dy[0] < TD5_TG_TOPO_FLORA_MINDROP) return 1;

    /* Somewhere on the falling part of the chain: past the skirt's outer point,
     * inside the ring that still carries relief. */
    {
        const double d0 = c.d[1] + 500.0;
        const double d1 = c.d[c.n - 1] * 0.45;
        if (!(d1 > d0)) return 1;
        d = d0 + (d1 - d0) * (double)((hh >> 11) % 1000u) * 0.001;
    }
    drop_in = tg_topo_drop_at(&c, d);

    if (td5_env_flag_on("TD5RE_R6_FLORA")) {
        v = tg_flora_upright_index(hh >> 13);
        if (v < 0) return 1;
    } else {
        v = (int)((hh >> 13) % (unsigned)k_r5_flora_tree_count);
    }
    page = tg_flora_tree_slot(v);

    jit = 1.00 + (double)((hh >> 9) % 61) * 0.01;
    tw  = 2600.0 * jit;
    th  = 4200.0 * jit;

    lx = nl->v[si].tz * side; lz = -nl->v[si].tx * side;
    {
        const double e = nl->v[si].width * 0.5 + d;
        cx = nl->v[si].x + lx * e;
        cz = nl->v[si].z + lz * e;
    }
    base_y = nl->v[si].y - drop_in;      /* ON the slope, not on its lip */

    /* [R12 item 4] shared spacing rule -- see tg_r12_flora_accept. */
    if (!tg_r12_flora_accept(si, "r9slope", page, side, cx, cz, tw, th))
        return 1;
    tg_flora_diag(nl, si, "r9slope", page, side, tw, th, cx, cz);
    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_emit_billboard_mesh(h->blk, cx, base_y, cz, tw * 0.5, th, page, 1))
        return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_R9_TOPO, si);
    return 1;
}

/* [R9 TOPO] CLASS SWEEP. Every span-side, both clauses that are geometric,
 * before/after in one number each -- and BOTH extents, because reporting the
 * horizontal one again is repeating exactly the measurement error that let item
 * 6 survive round 8. TD5RE_R9_TOPO_LOG=1. */
void tg_r9_topo_report(const TG_NodeList *nl, int nspans)
{
    const int ring = (s_ring_len > 0 && s_ring_len < nspans) ? s_ring_len : nspans;
    int si, s, sides = 0, v_open = 0, v_gap = 0, v_road = 0, v_mat = 0;
    double sum_h = 0.0, sum_s = 0.0, worst_open = 0.0, worst_road = 0.0;
    /* The three spans the user named, so the report photographs the spans the
     * complaint is about and not a convenient neighbour. Overridable. */
    const int at[3] = {
        td5_env_int("TD5RE_R9_TOPO_AT1",  549, 0, TD5_TG_MAX_SPANS),
        td5_env_int("TD5RE_R9_TOPO_AT2",  617, 0, TD5_TG_MAX_SPANS),
        td5_env_int("TD5RE_R9_TOPO_AT3", 1050, 0, TD5_TG_MAX_SPANS)
    };

    if (!td5_env_flag_off("TD5RE_R9_TOPO_LOG")) return;

    TD5_LOG_I(LOG_TAG, "trackgen: R9TOPO sweep enabled=%d ring=%d",
              tg_topo_enabled(), ring);
    for (si = 0; si + 1 < ring; si++) {
        /* C4 is a per-SPAN property of the surface material, not per side: the
         * ground page must not differ from BOTH neighbours (an isolated slab). */
        if (si > 0 && si + 2 < ring) {
            const int me = tg_topo_surface_page(si);
            if (me != tg_topo_surface_page(si - 1) &&
                me != tg_topo_surface_page(si + 1))
                v_mat++;
        }
        for (s = 0; s < 2; s++) {
            TG_TopoChain c;
            tg_topo_chain(nl, si, s, &c);
            sides++;
            sum_h += c.horiz;
            sum_s += c.surf;
            /* C2, stated as the EDGE'S OWN DEPRESSION rather than as its last
             * segment's grade. The first version of this test asked for
             * end_grade > RUNOUT and reported ZERO violations in both states,
             * which was true and useless: the R8 sink pins rings 2 and 3 at the
             * global floor, so the terminating segment is always FLAT. The
             * thing you can see is not the last segment, it is where the edge
             * SITS -- ground that has fallen `drop` and stops at `horiz` is a
             * cut edge seen at atan(drop/horiz) below the road, and the whole
             * point of the run-out is to push that angle onto the horizon. */
            if (!c.closed && (c.drop - c.dy[0]) > TD5_TG_TOPO_OPEN_DROP &&
                c.horiz > 1.0 &&
                (c.drop - c.dy[0]) / c.horiz > TD5_TG_TOPO_RUNOUT) {
                v_open++;
                if (c.drop > worst_open) worst_open = c.drop;
            }
            if (c.gap > TD5_TG_TOPO_GAP_TOL) v_gap++;
            if (c.road_cap < c.horiz - 1.0) {
                const double over = c.horiz - c.road_cap;
                v_road++;
                if (over > worst_road) worst_road = over;
            }
            if (si == at[0] || si == at[1] || si == at[2])
                TD5_LOG_I(LOG_TAG,
                    "trackgen: R9TOPO at si=%d %s biome=%s page=%d pts=%d "
                    "horiz=%.0f SURFACE=%.0f drop=%.0f closed=%d "
                    "road_cap=%.0f gap=%.0f end_grade=%.3f depress=%.3f",
                    si, s ? "L" : "R",
                    k_biomes[tg_topo_ground_index(si)].name,
                    tg_topo_surface_page(si), c.n,
                    c.horiz, c.surf, c.drop - c.dy[0], c.closed,
                    c.road_cap, c.gap, c.end_grade,
                    c.horiz > 1.0 ? (c.drop - c.dy[0]) / c.horiz : 0.0);
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: R9TOPO SUMMARY sides=%d | C1 gap=%d | C2 open-edge=%d "
        "(worst drop %.0f) | C3 road-overrun=%d (worst %.0f) "
        "| C4 isolated-surface-spans=%d "
        "| extent horiz mean=%.0f SURFACE mean=%.0f",
        sides, v_gap, v_open, worst_open, v_road, worst_road, v_mat,
        sides ? sum_h / (double)sides : 0.0,
        sides ? sum_s / (double)sides : 0.0);
}

/* ================= [R14 OVERPASS item 3] THE SURROUND, MEASURED ==============
 * "Wherever there is an overpass, always fill the nearby area with buildings and
 *  floor. Here the floor ends early and there is nothing behind it but
 *  background."  (seed 20260901, span 528)
 *
 * Three candidate causes were named before any of them was measured, so this
 * prints all three side by side, per crossing, per side, and lets the numbers
 * choose:
 *
 *   (a) THE R12 CLEAR CORRIDOR. tg_up_clear_span suppresses frontage, back rows,
 *       forkback and r13-fill over the deck footprint, and tg_up_xclear_span
 *       suppresses street intersections over a wider band. `sup=` counts the
 *       span-SIDES the corridor removed a wall from, so "the town is missing"
 *       can be attributed to the corridor or ruled out.
 *   (b) A R13 FOLD CLAMP on the ground reach. `fold=` is tg_r13_fold_reach's own
 *       answer at this span-side (1e30 on a straight and on the outside of every
 *       bend), so a truncated floor next to a bend is attributable rather than
 *       assumed.
 *   (c) GENUINELY UNOWNED GROUND. `arm=` is the deck's own lateral reach and
 *       `horiz=` is where the drawn ground actually stops (the whole topo chain:
 *       verge skirt plus far-band rings, the same authority the flora and the
 *       band read). `over=` is arm - horiz: how far the deck flies past the last
 *       drawn floor. That is the item's own words expressed as one number.
 *
 * Read-only: every quantity is a pure function of the span index and this calls
 * only predicates. Printed unconditionally -- a track carries a handful of
 * crossings and a measurement nobody prints is a measurement nobody checks. */
static double tg_r14_up_ground_horiz(const TG_NodeList *nl, int si, int is_left)
{
    TG_TopoChain c;
    tg_topo_chain(nl, si, is_left, &c);
    return (c.n > 0) ? c.horiz : 0.0;
}

/* The VERTICAL half of "the deck stands over a void". The soffit is a flat plane
 * TD5_TG_UP_CLEAR above the road, and the ground beneath it is not flat at all:
 * it drops TD5_TG_GROUND_DROP over the verge and then sinks toward the track
 * floor across the far-band rings. Sample the chain at quarters of the arm and
 * print the AIR GAP -- soffit height above the ground directly under it -- so
 * "there is nothing under the deck" is a height in units, not an impression. */
static void tg_r14_up_gap_line(const TG_NodeList *nl, int si, int is_left,
                               double arm_edge)
{
    TG_TopoChain c;
    double g[5];
    int k;
    tg_topo_chain(nl, si, is_left, &c);
    if (c.n <= 0) return;
    for (k = 0; k < 5; k++)
        g[k] = TD5_TG_UP_CLEAR
             + tg_topo_drop_at(&c, arm_edge * (double)k * 0.25);
    TD5_LOG_I(LOG_TAG,
        "trackgen: [R14 item 3] airgap @%d %s under-deck soffit-to-ground at "
        "0/25/50/75/100%% of arm (%.0f): %.0f %.0f %.0f %.0f %.0f",
        si, is_left ? "L" : "R", arm_edge, g[0], g[1], g[2], g[3], g[4]);
}

void tg_r14_up_report(const TG_NodeList *nl, int nspans)
{
    const int ring = (s_ring_len > 0 && s_ring_len < nspans) ? s_ring_len : nspans;
    /* [2026-09-04] MEASURED 3462 ms of a 22.8 s build (15.1%) -- the single
     * most expensive thing left in a generation, and it was ungated, so every
     * generated track paid for an overpass diagnostic nobody asked for. Now on
     * the same master gate as the other per-area sweeps (TD5RE_TG_REPORTS=1, or
     * TD5RE_R14_UP_REPORT=1 for this one alone). */
    if (!tg_report_wanted("TD5RE_R14_UP_REPORT")) return;
    int si, crossings = 0, sup_total = 0, folded = 0;
    double worst_over = 0.0;
    int worst_si = -1;

    for (si = 1; si + 1 < ring; si++) {
        const TG_Biome *bs = &k_biomes[tg_scenery_biome_index(si)];
        double half, shift;
        int s, sup = 0, fillsup = 0, k;

        if (tg_underpass_span(si) != si) continue;
        crossings++;
        tg_tunnel_bore(nl, si, &half, &shift);

        /* (a) what the clear corridor took out, span-side by span-side. */
        for (k = -TD5_TG_UP_CLEAR_SPANS; k <= TD5_TG_UP_CLEAR_SPANS; k++) {
            const int c = si + k;
            const TG_Biome *cb;
            if (c <= 0 || c + 1 >= ring) continue;
            cb = &k_biomes[tg_scenery_biome_index(c)];
            if (!(tg_city_sidewalk_w(cb) > 0.0)) continue;
            if (tg_span_in_bridge_run(c)) continue;
            for (s = 0; s < 2; s++) {
                if (tg_facade_built(c, s)) sup++;
                fillsup++;              /* r13-fill returns 0 on this whole band */
            }
        }
        sup_total += sup;

        for (s = 0; s < 2; s++) {
            const int is_left = s ? 1 : 0;
            const double arm  = tg_up_reach(nl, si, is_left, half, TD5_TG_UP_HALFDEEP);
            const double horiz = tg_r14_up_ground_horiz(nl, si, is_left);
            const double fold = tg_r13_fold_reach(nl, si, is_left ? 1.0 : -1.0, 0.0);
            const double tcap = tg_topo_road_cap(nl, si, is_left);
            /* The arm is measured from the BORE CENTRE and the ground from the
             * ROAD EDGE, so put the arm on the ground's own axis before
             * subtracting -- otherwise `over` carries the fork shift as error. */
            const double arm_edge = arm + (is_left ? shift : -shift)
                                  - tg_road_half_width(nl, si);
            const double over = arm_edge - horiz;
            int ridge = 1;
            const char *why = tg_r8_far_block_reason(nl, si,
                                  &k_biomes[tg_biome_for_span(si)], is_left, &ridge);
            if (fold < 1e29) folded++;
            if (over > worst_over) { worst_over = over; worst_si = si; }
            TD5_LOG_I(LOG_TAG,
                "trackgen: [R14 item 3] overpass @%d %s biome=%s sidewalk=%.0f "
                "arm=%.0f arm_edge=%.0f GROUND horiz=%.0f OVER=%.0f "
                "verge=%.0f far=%.0f fold=%.0f topo_cap=%.0f farband=%s ridge=%d "
                "corridor=%d..%d sup_sides=%d fill_sides=%d",
                si, is_left ? "L" : "R", bs->name, tg_city_sidewalk_w(bs),
                arm, arm_edge, horiz, over,
                tg_verge_reach(), tg_far_reach(),
                fold < 1e29 ? fold : -1.0, tcap,
                why ? why : "yes", ridge,
                si - TD5_TG_UP_CLEAR_SPANS, si + TD5_TG_UP_CLEAR_SPANS,
                sup, fillsup);
            tg_r14_up_gap_line(nl, si, is_left, arm_edge);
        }
        {   /* Occupancy map either side of the crossing: for each span in the
             * neighbourhood, what actually STANDS on each kerb. 'B' = a corner
             * block stands, 'p' = the run/gap pattern says built but something
             * suppressed it, '.' = open frontage, 'x' = the clear corridor. Read
             * left-to-right from si-6 to si+6, so "the deck sits in a hole in the
             * town" is a picture made of predicates rather than of pixels. */
            char mapL[16], mapR[16];
            int i;
            for (i = 0; i <= 12; i++) {
                const int c = si - 6 + i;
                for (s = 0; s < 2; s++) {
                    char *m = s ? mapL : mapR;
                    char v;
                    if (c <= 0 || c + 1 >= ring)        v = '?';
                    else if (tg_up_clear_span(c))       v = 'x';
                    else if (tg_r11_corner_stands(c, s))v = 'B';
                    else if (tg_facade_built(c, s))     v = 'p';
                    else                                v = '.';
                    m[i] = v;
                }
            }
            mapL[13] = mapR[13] = '\0';
            TD5_LOG_I(LOG_TAG,
                "trackgen: [R14 item 3] town @%d spans %d..%d L[%s] R[%s] "
                "(x=clear corridor, B=block stands, p=pattern only, .=open)",
                si, si - 6, si + 6, mapL, mapR);
            /* Why CASE C did or did not take each side of the band, per span.
             * The emitter's own clauses, in its own order -- a count alone
             * cannot say whether a refusal was the park test, the fork or the
             * yard roll, and the first two are correct while the third is a
             * dice throw the emitter may not want at a crossing mouth. This is
             * 14 lines per crossing, so it is the one part of the round's
             * evidence that is opt-in. td5_env_flag_OFF, not flag_on: the
             * latter returns 1 when the variable is UNSET and would make an
             * "opt-in" dump unconditional. */
            if (!td5_env_flag_off("TD5RE_R14_UP_REPORT")) continue;
            for (i = -TD5_TG_UP_XCLEAR; i <= TD5_TG_UP_XCLEAR; i++) {
                const int c = si + i;
                if (c <= 0 || c + 1 >= ring) continue;
                for (s = 0; s < 2; s++) {
                    const unsigned int rh = ((unsigned)c * 2654435761u
                                          + (unsigned)s * 374761393u) * 2246822519u;
                    TD5_LOG_I(LOG_TAG,
                        "trackgen: [R14 item 3] surround @%d span %d %s "
                        "near=%d stands=%d park=%d blocked=%d clear=%d yard=%d "
                        "-> %d",
                        si, c, s ? "L" : "R",
                        tg_r14_up_dist(c, TD5_TG_UP_XCLEAR) >= 0,
                        tg_r11_corner_stands(c, s), tg_block_is_park(c, s),
                        tg_side_blocked(c, s ? 1.0 : -1.0),
                        tg_up_clear_span(c), (rh >> 29) == 0u,
                        !tg_up_clear_span(c) && tg_r14_up_surround(c, s));
                }
            }
        }
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: [R14 item 3] SUMMARY crossings=%d suppressed_wall_sides=%d "
        "fold-clamped_sides=%d worst_deck_overhang=%.0f @%d",
        crossings, sup_total, folded, worst_over, worst_si);
}

void tg_r11_water_diag(const TG_NodeList *nl, int nspans)
{
    const int ring = (s_ring_len > 0 && s_ring_len < nspans) ? s_ring_len : nspans;
    const int at0 = td5_env_int("TD5RE_R11_WATER_AT1", 848, 0, TD5_TG_MAX_SPANS);
    const int at1 = td5_env_int("TD5RE_R11_WATER_AT2", 878, 0, TD5_TG_MAX_SPANS);
    const int pad = td5_env_int("TD5RE_R11_WATER_PAD",   8, 0, 200);
    const double BW = TD5_TG_BRIDGE_WATER_HALF;
    int si, worst_si = -1, nseam = 0, nbad = 0;
    double worst = 0.0, sum = 0.0;

    if (!td5_env_flag_off("TD5RE_R11_WATER_LOG")) return;

    TD5_LOG_I(LOG_TAG, "trackgen: R11WATER hdr si,biome,inrun,run0,run1,runwater,"
              "halfw,sea_outer,river_half,drawn_half,dheading_deg,seam_open,"
              "seam_side,coast_land_err");
    for (si = 0; si + 1 < ring; si++) {
        const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
        const TG_Node *n0 = &nl->v[si];
        const TG_Node *n1 = &nl->v[si + 1];
        /* Left unit at each node, exactly as both emitters form it. */
        const double lx0 = n0->tz, lz0 = -n0->tx;
        const double lx1 = n1->tz, lz1 = -n1->tx;
        /* With the fix in force each corner rides its own node's normal, so the
         * two spans meeting at node si+1 share their boundary points exactly and
         * the open seam is 0 by construction. Report what the CURRENT build
         * actually lays, so the same line is the before AND the after. */
        const double dl  = td5_env_flag_on("TD5RE_R11_WATER") ? 0.0
                         : sqrt((lx0 - lx1) * (lx0 - lx1) +
                                (lz0 - lz1) * (lz0 - lz1));
        const double dl_raw = sqrt((lx0 - lx1) * (lx0 - lx1) +
                                   (lz0 - lz1) * (lz0 - lz1));
        /* Which side the wedge OPENS on: the cross product of the two tangents
         * says which way the road turned, and the outside of the turn is the
         * side that loses coverage (the inside merely overlaps, and an overlap
         * of two coplanar quads at one y is invisible). */
        const double cross = n0->tx * n1->tz - n0->tz * n1->tx;
        const int inrun = tg_span_in_bridge_run(si);
        int run0 = -1, run1 = -1;
        double coast_err = 0.0, coast_reach = 0.0;

        if (!inrun) continue;                        /* only the river matters */
        tg_bridge_run_bounds(nl, si, &run0, &run1);
        if (!tg_water_span_clear(si)) continue;      /* no quad laid at all    */

        /* Coastline: its land corners are swept along the WATER node's normal,
         * so at the rim they miss the land node's own cross-section by this. */
        if (si == run1 && run1 + 2 < nl->count) {
            const TG_Node *nw = &nl->v[run1 + 1];
            const TG_Node *nn = &nl->v[run1 + 2];
            const double wx = nw->tz, wz = -nw->tx;
            const double nx = nn->tz, nz = -nn->tx;
            coast_err = td5_env_flag_on("TD5RE_R11_WATER") ? 0.0
                      : BW * sqrt((wx - nx) * (wx - nx) + (wz - nz) * (wz - nz));
        }
        coast_reach = tg_r11_wet_reach(nl, si);

        nseam++;
        sum += tg_r11_wet_reach(nl, si) * dl;
        if (tg_r11_wet_reach(nl, si) * dl > 200.0) nbad++;
        if (tg_r11_wet_reach(nl, si) * dl > worst) {
            worst = tg_r11_wet_reach(nl, si) * dl;
            worst_si = si;
        }

        if ((si >= at0 - pad && si <= at0 + pad) ||
            (si >= at1 - pad && si <= at1 + pad))
            TD5_LOG_I(LOG_TAG,
                "trackgen: R11WATER %d,%s,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,"
                "%.3f,%.0f,%s,%.0f",
                si, b->name, inrun, run0, run1, tg_bridge_run_is_water(nl, si),
                n0->width * 0.5, tg_r11_sea_outer(nl, si),
                coast_reach, coast_reach,
                2.0 * asin(dl_raw * 0.5 > 1.0 ? 1.0 : dl_raw * 0.5)
                    * 180.0 / TD5_TG_PI,
                coast_reach * dl,
                cross > 0.0 ? "R" : (cross < 0.0 ? "L" : "-"),
                coast_err);
    }
    TD5_LOG_I(LOG_TAG,
        "trackgen: R11WATER SUMMARY river-span-seams=%d | open seam at the rim "
        "(BW=%.0f): mean=%.0f worst=%.0f at si=%d | seams over 200u=%d",
        nseam, BW, nseam ? sum / (double)nseam : 0.0, worst, worst_si, nbad);
}

void tg_r8_terrain_extent_report(const TG_NodeList *nl, int nspans)
{
    /* Per-biome-kind accumulators of the effective extent, both sides.
     *
     * [R11 BIOME] Sized off TD5_TG_BIOME_KINDS, not TD5_TG_BIOME_COUNT. The two
     * are DIFFERENT NUMBERS and the difference is a stack overwrite: COUNT is 7
     * because it is the MODULUS of the normal biome draw (ALPTOWN is
     * deliberately excluded from it -- see the note on its table row), but these
     * four arrays are indexed by tg_biome_cell_index, which returns 7 for
     * ALPTOWN on every snow-coherent seed. So on any seed with snow this loop
     * wrote one past the end of four stack arrays and the summary below could
     * never print ALPTOWN's row. Opt-in dev diagnostic only, but it is a real
     * out-of-bounds write and it is triggered by a knob this round uses. */
    double sum[TD5_TG_BIOME_KINDS], worst[TD5_TG_BIOME_KINDS];
    int    cnt[TD5_TG_BIOME_KINDS], narrow[TD5_TG_BIOME_KINDS];
    int    blocked_fork = 0, blocked_bridge = 0, blocked_tunnel = 0;
    int    blocked_sea = 0, blocked_off = 0, sides = 0, narrow_all = 0;
    int    noridge = 0;
    const double far_reach = (double)td5_env_int("TD5RE_AUTOTRACK_TERRAIN_REACH",
                                                 TD5_TG_FAR_REACH, 30000, 400000);
    int si, s, k;

    /* Opt-in: td5_env_flag_off is 1 only when the var is explicitly "1", which is
     * the idiom the other trackgen DIAG knobs use (see the farband log). */
    if (!td5_env_flag_off("TD5RE_R8_TERRAIN_EXTENT_LOG")) return;

    for (k = 0; k < TD5_TG_BIOME_KINDS; k++) {
        sum[k] = 0.0; worst[k] = 1e30; cnt[k] = 0; narrow[k] = 0;
    }
    /* Every number below is only meaningful together with the knobs that were
     * in force, and TD5RE_* env vars survive for the life of the launching
     * shell -- an A/B run back-to-back can silently inherit the previous run's
     * settings. Print the state so a report line identifies its own build. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: ---- R8 terrain extent (knobs: EXTENT=%d SHORE=%d "
              "SNOW=%d TL_ASPECT=%d TL_VARY=%d) ----",
              td5_env_flag_on("TD5RE_R8_TERRAIN_EXTENT"),
              td5_env_flag_on("TD5RE_R8_TERRAIN_SHORE"),
              td5_env_flag_on("TD5RE_R8_TERRAIN_SNOW"),
              tg_r8_treeline_aspect(), tg_r8_treeline_vary());
    for (si = 0; si < nspans - 1 && si < TD5_TG_MAX_SPANS; si++) {
        const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
        const int bk = tg_biome_cell_index(si);
        const double wsd = b->water ? tg_water_side(si) : 0.0;
        for (s = 0; s < 2; s++) {
            const int is_left = s ? 1 : 0;
            const char *why;
            TG_GroundProf p;
            double skirt, ext;
            int ridge = 1;

            tg_ground_side(nl, si, is_left, wsd, &p);
            skirt = p.d[p.n - 1];
            why = tg_r8_far_block_reason(nl, (si / TD5_TG_FAR_GROUP)
                                             * TD5_TG_FAR_GROUP,
                                         b, is_left, &ridge);
            ext = why ? skirt : far_reach;
            if (!ridge) noridge++;
            sides++;
            /* The reason strings are the file-scope literals returned by
             * tg_r8_far_block_reason, so identity is the right test; strcmp
             * here trips -Wstring-compare on the folded constant lengths. */
            if (why) {
                if      (why == k_r8_why_fork)   blocked_fork++;
                else if (why == k_r8_why_bridge) blocked_bridge++;
                else if (why == k_r8_why_tunnel) blocked_tunnel++;
                else if (why == k_r8_why_sea)    blocked_sea++;
                else                             blocked_off++;
            }
            if (ext < 20000.0) { narrow[bk]++; narrow_all++; }
            sum[bk] += ext; cnt[bk]++;
            if (ext < worst[bk]) worst[bk] = ext;
            /* Per-span detail only around the spans the user named, plus every
             * 100th span, so the log stays readable on a 1800-span track. */
            if ((si >= 560 && si <= 580) || (si >= 190 && si <= 215) ||
                (si % 100) == 0)
                TD5_LOG_I(LOG_TAG,
                          "  extent si=%4d %s biome=%-11s skirt=%7.0f "
                          "far=%s%s%s ridge=%d ext=%8.0f over=%6.0f",
                          si, is_left ? "L" : "R", b->name, skirt,
                          why ? "NO(" : "YES", why ? why : "", why ? ")" : "",
                          ridge, ext,
                          tg_carriageway_reach(nl, si, is_left ? 1.0 : -1.0)
                              - tg_road_half_width(nl, si));
        }
    }
    for (k = 0; k < TD5_TG_BIOME_KINDS; k++) {
        if (!cnt[k]) continue;
        TD5_LOG_I(LOG_TAG,
                  "  extent BIOME %-11s sides=%5d mean=%8.0f min=%8.0f "
                  "narrow(<20000)=%d",
                  k_biomes[k].name, cnt[k], sum[k] / cnt[k], worst[k], narrow[k]);
    }
    TD5_LOG_I(LOG_TAG,
              "  extent TOTAL sides=%d narrow=%d (%.1f%%) | ground-blocked "
              "fork=%d bridge=%d tunnel=%d sea=%d off=%d | ridge-only-dropped=%d",
              sides, narrow_all, sides ? 100.0 * narrow_all / sides : 0.0,
              blocked_fork, blocked_bridge, blocked_tunnel, blocked_sea,
              blocked_off, noridge);
    /* [R8 item 14] The stretch and repetition numbers, measured off the ridges
     * that were actually written. ratio = tile world width / tile world height;
     * 1.0 means the square page is drawn square. */
    if (s_r8_tl_n) {
        int v;
        TD5_LOG_I(LOG_TAG,
                  "  treeline ridges=%d tile_w=%.0f tile_h=%.0f ratio=%.2f "
                  "(min %.2f max %.2f) aspect=%d vary=%d",
                  s_r8_tl_n, s_r8_tl_w / s_r8_tl_n, s_r8_tl_h / s_r8_tl_n,
                  (s_r8_tl_h > 0.0) ? s_r8_tl_w / s_r8_tl_h : 0.0,
                  s_r8_tl_ratio_min, s_r8_tl_ratio_max,
                  tg_r8_treeline_aspect(), tg_r8_treeline_vary());
        for (v = 0; v <= TD5_TG_R8_TREELINE_N; v++)
            if (s_r8_tl_pages[v])
                TD5_LOG_I(LOG_TAG, "  treeline page %s%d used by %d ridges",
                          (v == TD5_TG_R8_TREELINE_N) ? "LEGACY" : "variant ",
                          (v == TD5_TG_R8_TREELINE_N) ? 0 : v, s_r8_tl_pages[v]);
    }
    /* [R8 item 16] Snow appearance, per BIOME CELL rather than per span, because
     * the cell is the unit the variant is chosen on. Proves three things at
     * once: every snowy cell is on a snow page; the pages are not all the same
     * one; and the median page differs from the ground page on every one. */
    {
        int cell, nsnow = 0, nmed_ok = 0, nvar[TD5_TG_R8_SNOWGND_N + 1];
        for (cell = 0; cell <= TD5_TG_R8_SNOWGND_N; cell++) nvar[cell] = 0;
        for (cell = 0; cell * TD5_TG_BIOME_RUN < nspans; cell++) {
            const int cs = cell * TD5_TG_BIOME_RUN;
            const TG_Biome *cb = &k_biomes[tg_biome_cell_index(cs)];
            int gp, mp, probe = -1, j;
            if (!tg_biome_is_snow(cb)) continue;
            /* A tunnel span keeps the biome page by design, so probe the first
             * NON-tunnel span of the cell -- otherwise the report would read a
             * deliberate exemption as a miss. */
            for (j = cs; j < cs + TD5_TG_BIOME_RUN && j < nspans; j++)
                if (!tg_span_in_tunnel(j) && !tg_span_in_bridge_run(j)) {
                    probe = j; break;
                }
            if (probe < 0) continue;
            gp = tg_ground_page_for_span(probe, cb);
            mp = tg_r8_median_page(probe, cb->ground_page);
            nsnow++;
            if (mp != gp && mp != cb->ground_page) nmed_ok++;
            if (gp >= TD5_TG_PAGE_R8_SNOWGND &&
                gp <  TD5_TG_PAGE_R8_SNOWGND + TD5_TG_R8_SNOWGND_N)
                nvar[gp - TD5_TG_PAGE_R8_SNOWGND]++;
            else
                nvar[TD5_TG_R8_SNOWGND_N]++;
            TD5_LOG_I(LOG_TAG,
                      "  snow cell %2d spans %4d-%4d %-11s ground_page=%d "
                      "median_page=%d biome_page=%d distinct=%d",
                      cell, cs, cs + TD5_TG_BIOME_RUN - 1, cb->name,
                      gp, mp, cb->ground_page,
                      (mp != gp && mp != cb->ground_page));
        }
        if (nsnow)
            TD5_LOG_I(LOG_TAG,
                      "  snow TOTAL cells=%d median-distinct=%d | ground "
                      "variant0=%d variant1=%d variant2=%d legacy=%d",
                      nsnow, nmed_ok, nvar[0], nvar[1], nvar[2],
                      nvar[TD5_TG_R8_SNOWGND_N]);
    }

    /* [R11 BIOME item 3] The MEDIAN of every fork, and the fact the R8 snow-cell
     * report above cannot see: that report probes the first NON-tunnel span of a
     * cell, so a cell reads "median-distinct" while a fork anchored ON a tunnel
     * span inside it paints its whole gore green. One line per fork -- anchor,
     * whether the anchor is enclosed, and the page actually chosen -- so the
     * failure is readable straight from the log instead of inferred. */
    {
        int f;
        for (f = 0; f < s_fork_count; f++) {
            const int    fs = s_forks[f].F;
            const TG_Biome *fb = &k_biomes[tg_biome_cell_index(fs)];
            const int    bore = tg_span_in_tunnel(fs);
            const int    now  = tg_fork_gore_page(f);
            const int    old  = tg_r8_median_page(fs, fb->ground_page);
            TD5_LOG_I(LOG_TAG,
                      "  median fork %d anchor=%4d %-11s snow=%d "
                      "anchor_in_bore=%d page=%d (anchor-tested=%d) %s",
                      f, fs, fb->name, tg_biome_is_snow(fb), bore, now, old,
                      (now != old) ? "<- R11 item 3 fixed here" : "");
        }
    }

    /* [R11 BIOME item 4] The OUTSKIRTS ramp, measured rather than described.
     * One line per wilderness-to-town edge naming the boundary span and the
     * band, then a per-block breakdown of the three ramped quantities -- how
     * many of the 2 x PERIOD candidate frontages the ramp opened, and the mean
     * front-wall height -- so "it ramps" is a table and not a claim. Also prints
     * the FIRST CORE block past the band as the control row. */
    {
        int cell;
        for (cell = 1; cell * TD5_TG_BIOME_RUN < nspans; cell++) {
            const int cs = cell * TD5_TG_BIOME_RUN;
            int a, z, blk;
            if (!tg_biome_index_is_paved(tg_biome_cell_index(cs))) continue;
            tg_biome_run_bounds(cs, &a, &z);
            if (a != cs) continue;                  /* not this run's first cell */
            if (tg_biome_index_is_paved(tg_biome_cell_index(a - 1))) continue;
            TD5_LOG_I(LOG_TAG,
                      "  town edge at span %4d: %-11s after %-11s, ramp band "
                      "%d-%d (knob TD5RE_R11_TOWN_RAMP=%s)",
                      a, k_biomes[tg_biome_cell_index(a)].name,
                      k_biomes[tg_biome_cell_index(a - 1)].name,
                      a, a + TD5_TG_TOWN_RAMP - 1,
                      td5_env_flag_on("TD5RE_R11_TOWN_RAMP") ? "on" : "off");
            for (blk = 0; blk <= TD5_TG_TOWN_RAMP / TD5_TG_FACADE_PERIOD; blk++) {
                const int b0 = a + blk * TD5_TG_FACADE_PERIOD;
                int si, s, open = 0, cand = 0, hsum = 0, hn = 0;
                for (si = b0; si < b0 + TD5_TG_FACADE_PERIOD && si < nspans; si++)
                    for (s = 0; s < 2; s++) {
                        if (!tg_facade_built(si, s)) continue;
                        cand++;
                        if (tg_town_ramp_open(si, s)) { open++; continue; }
                        hsum += tg_facade_floors(si, s,
                                    &k_biomes[tg_scenery_biome_index(si)]);
                        hn++;
                    }
                TD5_LOG_I(LOG_TAG,
                          "    block %d spans %4d-%4d ramp=%.2f frontages "
                          "%d/%d open (%d stand) mean_floors=%.2f%s",
                          blk, b0, b0 + TD5_TG_FACADE_PERIOD - 1,
                          tg_town_ramp(b0), open, cand, hn,
                          hn ? (double)hsum / (double)hn : 0.0,
                          (blk * TD5_TG_FACADE_PERIOD >= TD5_TG_TOWN_RAMP)
                              ? "  <- CORE (control row)" : "");
            }
        }
    }

    /* A race launch builds the level more than once; clear so the second report
     * measures its own build rather than the sum of both. */
    s_r8_tl_w = s_r8_tl_h = 0.0; s_r8_tl_n = 0;
    memset(s_r8_tl_pages, 0, sizeof(s_r8_tl_pages));
}

/* ===================== [R9 BRIDGE] TIE + DRY-BAND EVIDENCE =====================
 * Round 9's two items are both CLASS claims -- "every over-deck member lands on
 * a pier, on every run, on both seeds" and "no background massing stands over
 * water anywhere". A frame can only ever show one span of one run, and the R9
 * method note is explicit that a count going up is not evidence of correct
 * placement. So this reports the UNIQUENESS/COVERAGE facts directly:
 *
 *   per run: style, pier pitch, pier count, gantry count, crown span, and
 *            ORPHANS -- over-deck members (gantry or tower) standing on a span
 *            that carries no pier, and the worst lateral offset between an
 *            over-deck leg and the pier leg beneath it. Both must be 0.
 *   overall: far-band sides tested / clamped / dropped by the over-water rule.
 *
 * Always logged (it is a handful of lines per track and it is the acceptance
 * evidence); TD5RE_R9_BRIDGE_REPORT=0 silences it. */
void tg_r9_bridge_report(const TG_NodeList *nl)
{
    int si, runs = 0, orphan_total = 0;
    double worst_lat = 0.0;

    if (!tg_report_wanted("TD5RE_R9_BRIDGE_REPORT")) return;

    for (si = 0; si + 1 < nl->count; si++) {
        int s0, s1, s, piers = 0, gantries = 0, orphans = 0, crown, style, pitch;
        double wl = 0.0;
        if (!tg_span_in_bridge_run(si)) continue;
        tg_bridge_run_bounds(nl, si, &s0, &s1);
        if (si != s0) continue;                     /* once per run */
        if (s1 > nl->count - 2) s1 = nl->count - 2;
        style = tg_bridge_style(si);
        pitch = tg_bridge_pier_pitch(style);
        crown = s0 + (s1 - s0) / 2;
        for (s = s0; s <= s1; s++) {
            const int pier = tg_bridge_pier_here(nl, s);
            const int gan  = tg_bridge_gantry_here(nl, s);
            const int tower = tg_bridge_struct_enabled() && style != 2 &&
                              s == crown;
            if (pier) piers++;
            if (gan)  gantries++;
            if ((gan || tower) && !pier) orphans++;
            if ((gan || tower) && pier) {
                /* The two laterals written EXACTLY as the two emitters compute
                 * them, so this measures the real gap rather than asserting it.
                 * Under the tie both reduce to tg_bridge_column_lateral and the
                 * difference is 0 by construction; with TD5RE_R9_BRIDGE_TIE=0 it
                 * reports the round-8 mismatch. */
                const double half = nl->v[s].width * 0.5;
                const double pierlat = tg_r9_bridge_tie()
                    ? tg_bridge_column_lateral(nl, s, 1.0)
                    : half * ((style == 1) ? 0.85 : 0.70);
                const double abovelat = tg_r9_bridge_tie()
                    ? tg_bridge_column_lateral(nl, s, 1.0)
                    : (gan ? tg_carriageway_reach(nl, s, 1.0)
                             + TD5_TG_BRIDGE_RAIL_OUT
                           : half + 300.0);
                if (fabs(abovelat - pierlat) > wl) wl = fabs(abovelat - pierlat);
            }
        }
        runs++;
        orphan_total += orphans;
        if (wl > worst_lat) worst_lat = wl;
        TD5_LOG_I(LOG_TAG,
            "R9BRIDGE run=%d-%d style=%d pitch=%d piers=%d gantries=%d "
            "crown=%d crown_has_pier=%d orphans=%d",
            s0, s1, style, pitch, piers, gantries, crown,
            tg_bridge_pier_here(nl, crown), orphans);
    }
    TD5_LOG_I(LOG_TAG,
        "R9BRIDGE tie=%d tex=%d coast=%d dryband=%d | runs=%d "
        "over_deck_orphans=%d worst_leg_lat=%.0f | farband sides=%d "
        "clamped=%d dropped=%d mean_clamp=%.0f",
        tg_r9_bridge_tie(), tg_r9_bridge_tex(),
        td5_env_flag_on("TD5RE_R9_BRIDGE_COAST"), tg_r9_dryband_enabled(),
        runs, orphan_total, worst_lat,
        s_r9_band_tested, s_r9_band_clamped, s_r9_band_dropped,
        s_r9_band_clamped ? s_r9_band_clamp_sum / (double)s_r9_band_clamped
                          : 0.0);
    {   /* [R9 item 10] the CLASS test: nothing but the crossing itself may stand
         * above the river surface, anywhere on the track. */
        int k;
        char kinds[256];
        size_t w = 0;
        kinds[0] = 0;
        for (k = 0; k < TG_GK_COUNT; k++) {
            if (!s_r9_wet_kind[k]) continue;
            w += (size_t)snprintf(kinds + w, sizeof(kinds) - w, "%s%s=%d",
                                  w ? " " : "", k_guard_kind_name[k],
                                  s_r9_wet_kind[k]);
            if (w >= sizeof(kinds) - 24) break;
        }
        TD5_LOG_I(LOG_TAG,
            "R9BRIDGE over-water found=%d rejected=%d remaining=%d "
            "worst_height=%.0f first_span=%d [%s]",
            s_r9_wet_total, s_r9_wet_rejected,
            s_r9_wet_total - s_r9_wet_rejected, s_r9_wet_worst,
            s_r9_wet_first_span, s_r9_wet_total ? kinds : "none");
    }
    s_r9_band_tested = s_r9_band_clamped = s_r9_band_dropped = 0;
    s_r9_band_clamp_sum = 0.0;
    s_r9_wet_total = 0; s_r9_wet_worst = 0.0; s_r9_wet_first_span = -1;
    s_r9_wet_ready = 0; s_r9_wet_rejected = 0;
    memset(s_r9_wet_kind, 0, sizeof(s_r9_wet_kind));
}
