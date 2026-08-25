/**
 * td5_trackgen.c -- procedural ("AUTO-GENERATED") track builder (PORT-ONLY).
 *
 * See td5_trackgen.h for the contract. Pipeline:
 *
 *   seeded RNG -> section picker -> centerline nodes -> curvature safety
 *              -> elevation profile -> spans + vertex rows -> STRIP.DAT
 *                                                          -> LEFT/RIGHT.TRK
 *                                                          -> LEVELINF.DAT
 *
 * WHY FINITE (Phase 1) AND NOT STREAMED: appending spans mid-race is blocked by
 * four independent things, none cosmetic --
 *   1. TD5_TrackProbe.span_index is int16 (td5_types.h), so span indices cannot
 *      grow without bound; an endless road needs an index rebase.
 *   2. LEFT/RIGHT.TRK route tables are sized to the ring at load and have no
 *      realloc path; appending spans without route bytes reads out of bounds
 *      (the failure documented at td5_track.c:3503).
 *   3. The minimap builds its segment table once at init from the whole ring
 *      (td5_hud.c), so a growing track needs a sliding/rescaling minimap.
 *   4. AI span arithmetic wraps at +/- span_count/2 (td5_ai.c smart_span_gap),
 *      which sign-flips on a track whose length changes underneath it.
 * Generating a long finite track up front sidesteps all four: every consumer
 * sees an ordinary point-to-point track with a known length.
 *
 * Coordinates are raw signed world units (the renderer divides by 256).
 */
#include "td5_trackgen.h"
#include "td5_track_registry.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_tg_real_tex.h"   /* real TD5 texture pages (level014), opt-in */
#include "td5re.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <direct.h>

#define LOG_TAG "track"

/* Reserved identity. Level 90 is well clear of the shipped levels (0..40ish)
 * and of td5_trackgen.py's DEFAULT_CUSTOM_LEVEL_BASE (40) so a user's
 * hand-built tracks and the auto track can coexist. */
#define TD5_TG_LEVEL_NUM   90
/* Last custom slot, so manifest-loaded tracks keep the low slots. */
#define TD5_TG_SLOT        (TD5_CUSTOM_TRACK_SLOT_BASE + TD5_CUSTOM_TRACK_MAX - 1)

/* Span the start/finish line sits on. The grid places the six racers STAGGERED
 * BEHIND the start line, so starting at span 0 puts the back of the grid on
 * negative spans that do not exist on a point-to-point track (observed:
 * span=-9, no wheel contact). Leave road behind the line. */
#define TD5_TG_GRID_SPAN   24

/* STRIP.DAT pre-span block: 5 header DWORDs + 196 bytes of jump-table space. */
#define TD5_TG_PRE_SPAN_BYTES  196
#define TD5_TG_SPAN_OFFSET     (20 + TD5_TG_PRE_SPAN_BYTES)   /* 216 */

/* Vertex indices in a span record are u16, and each span emits 2*(lanes+1)
 * vertices, so the vertex table -- not the span count -- is the binding
 * ceiling. Keep a margin under 65535. */
/* Spans per shared origin block. Bounded by the int16 vertex offset: the last
 * row of a block sits TD5_TG_ORIGIN_BLOCK * span_length down-track from the
 * origin, plus half the widest road, and that must stay under 32767.
 * 16 * 1500 + ~7500 = 31500, inside 32767 (the emitter hard-checks anyway).
 * Shipped level001 uses runs of ~22 spans per origin. */
#define TD5_TG_ORIGIN_BLOCK   16

/* Span byte flags copied from shipped data (level001: every road span is
 * [type 1, attr 0x11, mask 0x09, packed 0x84]).
 *   attr  0x11 = primary surface 1 / alternate surface 1. Surface type selects
 *                GRIP (td5_track.c:3837); type 0 -- what this generator wrote
 *                at first -- is not the tarmac class shipped roads use.
 *   mask  bit N = lane N uses the alternate surface; shipped marks the two
 *                outer lanes (0x09 for a 4-lane road).
 *   packed high nibble 8 = span_height_offset, only ever read as a DIFFERENCE
 *                between neighbours, so a uniform value is what matters. */
#define TD5_TG_SURFACE_ATTR   0x11
#define TD5_TG_HEIGHT_NIBBLE  8

/* Texture page ids, in the order tg_emit_textures writes them. Declared here
 * because the mesh emitters (further up) reference them. */
#define TD5_TG_PAGE_ROAD   0
#define TD5_TG_PAGE_WALL   1   /* facade variant 0 */
#define TD5_TG_PAGE_GREEN  2
#define TD5_TG_PAGE_TREE   3
#define TD5_TG_PAGE_RAIL   4
#define TD5_TG_PAGE_GROUND 5
/* Facade variety: each RUN picks one of N wall pages so a street is not one
 * repeated building. Variant 0 is TD5_TG_PAGE_WALL; variants 1..N-1 live at
 * consecutive pages after GROUND. */
#define TD5_TG_WALL_VARIANTS   5
#define TD5_TG_PAGE_WALL_EXTRA 6
/* Storefronts: the GROUND floor of a facade uses a shop page (glass/signage),
 * upper floors the wall page -- shops at street level, tower above, as the
 * shipped city does. Store variants live right after the wall variants. */
#define TD5_TG_STORE_VARIANTS  3
#define TD5_TG_PAGE_STORE  (TD5_TG_PAGE_WALL_EXTRA + TD5_TG_WALL_VARIANTS - 1)
/* Thematic trees: a set of distinct tree/palm/conifer/topiary pages so each
 * biome mixes several species. Variant 0 reuses TD5_TG_PAGE_TREE; 1..N-1 live
 * after the store pages. */
#define TD5_TG_TREE_VARIANTS   10
#define TD5_TG_PAGE_TREE_EXTRA (TD5_TG_PAGE_STORE + TD5_TG_STORE_VARIANTS)
#define TD5_TG_PAGE_COUNT  (TD5_TG_PAGE_TREE_EXTRA + TD5_TG_TREE_VARIANTS - 1)

#define TD5_TG_MAX_VERTICES   64000
#define TD5_TG_MAX_SPANS      3000

/* Minimum turn radius as a multiple of the road's half-width. Mirrors
 * td5_trackgen.py's CURVE_SAFETY_DEFAULT (1.5); the extra 1.2 is headroom so
 * the resampled centerline never lands exactly on the floor. */
#define TD5_TG_CURVE_SAFETY   (1.5 * 1.2)
/* Steepest allowed |dY/d(arc)|, mirroring td5_trackgen.py's max_grade.
 * Was briefly cut to 0.035 on an inverted reading of the airborne mask (see
 * the row-order note in tg_emit_strip); restored to the Python tool's value. */
#define TD5_TG_MAX_GRADE      0.12

#define TD5_TG_PI 3.14159265358979323846

/* ---------------------------------------------------------------- RNG ----- */
/* Private xorshift32 -- deliberately NOT rand(). The game's rand() is the
 * MSVC-compatible one used for sim determinism and netplay lockstep
 * (td5_msvc_rand.c); drawing track geometry from it would perturb every
 * downstream random draw and break trace goldens. */
static unsigned int s_rng;

/* Seed to run the S2 regen gate against, set during a build and consumed after
 * it finishes (the gate itself rebuilds a centerline, so it cannot run from
 * inside one). 0 = nothing to check. */
static unsigned int s_selfcheck_regen_seed = 0;

/* Branch jump-table state, produced by tg_emit_strip and consumed by the
 * header write in the same call. */
static int s_jump_lo, s_jump_hi, s_jump_base, s_jump_have, s_ring_len;

/* Seed of the last successful build, for reproducing a good random track. */
static unsigned int s_last_seed = 0;

static void tg_srand(unsigned int seed)
{
    s_rng = seed ? seed : 0x9E3779B9u;
}

static unsigned int tg_rand(void)
{
    unsigned int x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* Uniform in [lo, hi] inclusive. */
static int tg_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(tg_rand() % (unsigned int)(hi - lo + 1));
}

/* Uniform in [0, 1). */
static double tg_frand(void)
{
    return (double)(tg_rand() >> 8) / 16777216.0;
}

/* ------------------------------------------------------- centerline ------- */
typedef struct {
    double x, y, z;      /* world units */
    double width;        /* full road width, world units */
    int    lanes;        /* 1..12 */
    double tx, tz;       /* unit tangent (filled after the walk) */
} TG_Node;

typedef struct {
    TG_Node *v;
    int      count;
    int      cap;
} TG_NodeList;

static int tg_nodes_reserve(TG_NodeList *nl, int need)
{
    if (need <= nl->cap) return 1;
    {
        int cap = nl->cap ? nl->cap : 256;
        TG_Node *nv;
        while (cap < need) cap *= 2;
        nv = (TG_Node *)realloc(nl->v, (size_t)cap * sizeof(TG_Node));
        if (!nv) return 0;
        nl->v = nv;
        nl->cap = cap;
    }
    return 1;
}

static int tg_nodes_push(TG_NodeList *nl, double x, double z,
                         double width, int lanes)
{
    if (!tg_nodes_reserve(nl, nl->count + 1)) return 0;
    {
        TG_Node *n = &nl->v[nl->count++];
        n->x = x; n->y = 0.0; n->z = z;
        n->width = width;
        n->lanes = lanes;
        n->tx = 0.0; n->tz = 1.0;
    }
    return 1;
}

/* --- self-intersection guard -------------------------------------------
 * A random 2D walk WILL cross itself, and the engine localises a car to a
 * span by proximity -- so where the road overlaps, the span walker snaps to
 * the wrong span and spawn/progress/ground-probe all break (observed: car
 * placed at span 1791 of 1800 on the start line, airborne).
 *
 * Two roads overlap when their centerlines are closer than the sum of their
 * half-widths, so that -- plus a quarter-lane epsilon -- is the exact test.
 * Nodes within an ADJACENT-SKIP window of each other along the road are exempt
 * (a legal tight turn genuinely brings the road near itself); beyond that
 * window the heading budget below makes overlap geometrically IMPOSSIBLE, so
 * the check there is a pure backstop that never fires. The skip is DERIVED
 * from the budget at build time (tg_adjacent_skip) rather than hardcoded, so
 * the guarantee holds no matter how sharp the acute budget is set -- a
 * previously-hardcoded 25 would silently under-exempt once the budget rose.
 */

/* Widest road the generator can emit: the DUAL_LANE section caps lanes at 12
 * (see tg_build_centerline), so this bounds the too-close "need" distance and
 * therefore the derived adjacent-skip. */
#define TD5_TG_MAX_LANES 12

/* Heading budget, radians, measured from TD5_TG_AXIS_HEADING. This is what
 * makes the walk NON-TRAPPING: with |dev| <= limit every span advances the
 * axis coordinate by at least span_length*cos(limit) > 0, so that coordinate
 * is strictly increasing and two nodes far enough apart in span index can
 * never coincide -- self-intersection is geometrically impossible, not merely
 * rejected. Pure rejection sampling was tried first and traps: a self-avoiding
 * 2D walk paints itself into a cul-de-sac (observed: 1800 requested, then 300).
 *
 *   SPINE (~80 deg): straight/curve/dual-lane. Swings -80..+80 = a 160 deg
 *         switchback.
 *   ACUTE (~88 deg, tunable): tight sections only. Swings up to ~176 deg, a
 *         near-hairpin. cos(88 deg) is still > 0, so forward progress -- and
 *         the non-trapping proof -- survive; the price is a larger derived
 *         skip. A TRUE >=180 deg down-track hairpin is INCOMPATIBLE with a
 *         single-axis monotone guarantee (cos <= 0 there) and is deliberately
 *         NOT offered. TD5RE_AUTOTRACK_ACUTE_DEG (80..89) tunes the acute
 *         budget; the derived skip uses whichever limit is larger. */
#define TD5_TG_HEADING_LIMIT       1.396   /* spine, ~80 deg */
#define TD5_TG_ACUTE_HEADING_DEG   88      /* default acute budget, degrees */
#define TD5_TG_ACUTE_HEADING_MIN   80      /* never below the spine */
#define TD5_TG_ACUTE_HEADING_MAX   89      /* keep cos(limit) > 0 (non-trapping) */

/* Global axis the walk wanders about, radians. Deliberately +X (90 deg) rather
 * than +Z (0 deg): route byte[1] encodes the ABSOLUTE 12-bit heading as
 * heading = (byte * 0x102C) >> 8 (td5_ai.c:1280), and a byte < 4 is a junction
 * sentinel rather than a heading. With the axis at 0 the commonest heading
 * (straight ahead) would encode to byte 0..3 and be read as a sentinel; at
 * 90 deg the +/-80 deg spine band maps to bytes 7..120, clear of it. The
 * wider acute band reaches down toward byte 4 at its sharpest left-hand apex;
 * tg_emit_routes clamps to >=4 there, a small heading-fidelity loss on a
 * handful of apex spans rather than a sentinel collision. */
#define TD5_TG_AXIS_HEADING (TD5_TG_PI * 0.5)

static int tg_too_close(const TG_NodeList *nl, double x, double z,
                        double width, double lane_width, int skip)
{
    const int limit = nl->count - skip;
    int i;
    for (i = 0; i < limit; i++) {
        double dx = nl->v[i].x - x;
        double dz = nl->v[i].z - z;
        double need = (nl->v[i].width + width) * 0.5 + lane_width * 0.25;
        if (dx * dx + dz * dz < need * need) return 1;
    }
    return 0;
}

/* Acute heading budget in radians, from the env knob, clamped so cos(limit)>0
 * (the non-trapping proof requires strictly-positive forward progress). */
static double tg_acute_heading_limit(void)
{
    int deg = td5_env_int("TD5RE_AUTOTRACK_ACUTE_DEG", TD5_TG_ACUTE_HEADING_DEG,
                          TD5_TG_ACUTE_HEADING_MIN, TD5_TG_ACUTE_HEADING_MAX);
    return (double)deg * TD5_TG_PI / 180.0;
}

/* Derived adjacent-skip window (see the self-intersection guard note): the
 * smallest N such that N spans of guaranteed forward progress exceed the
 * widest possible too-close "need", so any two nodes >= N apart provably
 * cannot overlap. limit_max is the largest heading budget any section may use,
 * so cos(limit_max) is the GLOBAL minimum forward progress per span. */
static int tg_adjacent_skip(const TD5_TrackGenSpec *spec, double limit_max)
{
    double span_len   = (double)spec->span_length;
    double lane_width = (double)spec->lane_width;
    double w_max      = (double)TD5_TG_MAX_LANES * lane_width;
    /* Worst-case need in tg_too_close: both roads at the max width. */
    double need_max   = w_max + lane_width * 0.25;
    double dmin       = span_len * cos(limit_max);   /* min forward progress/span */
    int skip;
    if (dmin < 1.0) dmin = 1.0;                       /* guard against cos -> 0 */
    skip = (int)ceil(need_max / dmin);
    if (skip < 1) skip = 1;
    return skip;
}

/* Pick a section type from the normalised weights. */
static TD5_TrackGenSection tg_pick_section(const TD5_TrackGenSpec *spec)
{
    int total = 0, i, roll;
    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
        int w = spec->weight[i];
        if (w > 0) total += w;
    }
    if (total <= 0) return TD5_TG_STRAIGHT;
    roll = tg_range(0, total - 1);
    for (i = 0; i < TD5_TG_SECTION_COUNT; i++) {
        int w = spec->weight[i];
        if (w <= 0) continue;
        if (roll < w) return (TD5_TrackGenSection)i;
        roll -= w;
    }
    return TD5_TG_STRAIGHT;
}

static const char *tg_section_name(TD5_TrackGenSection s)
{
    switch (s) {
        case TD5_TG_STRAIGHT:  return "straight";
        case TD5_TG_CURVE:     return "curve";
        case TD5_TG_ACUTE:     return "acute";
        case TD5_TG_DUAL_LANE: return "dual-lane";
        default:               return "?";
    }
}

/* Walk the section picker until target_spans spans' worth of nodes exist.
 * Node i and i+1 bracket span i, so we need target_spans+1 nodes. */
static int tg_build_centerline(const TD5_TrackGenSpec *spec, TG_NodeList *nl,
                              int section_tally[TD5_TG_SECTION_COUNT])
{
    const int    want_nodes = spec->target_spans + 1;
    const double span_len   = (double)spec->span_length;
    const double base_width = (double)spec->lanes * (double)spec->lane_width;

    double x = 0.0, z = 0.0;
    /* Absolute heading; 0 = +Z. Wanders within +/-TD5_TG_HEADING_LIMIT of
     * TD5_TG_AXIS_HEADING, which keeps the walk non-trapping AND keeps the
     * route heading byte clear of the junction sentinel. */
    double heading = TD5_TG_AXIS_HEADING;
    double width = base_width;        /* current, ramped toward target_width */
    /* Max width change per span so widenings taper instead of stepping. */
    const double width_ramp = (double)spec->lane_width * 0.5;

    /* Heading budgets and the adjacent-skip derived from them. ACUTE sections
     * get a sharper budget (see TD5_TG_ACUTE_HEADING_DEG); the derived skip is
     * sized off the LARGER budget so the non-trapping / no-self-intersection
     * guarantee holds across all section types. */
    const double acute_limit = tg_acute_heading_limit();
    const double limit_max    = (acute_limit > TD5_TG_HEADING_LIMIT)
                              ? acute_limit : TD5_TG_HEADING_LIMIT;
    const int    skip         = tg_adjacent_skip(spec, limit_max);
    /* Round, do not truncate. The spine limit is the literal 1.396 rad =
     * 79.985 deg, and an 88 deg acute budget round-trips through radians to
     * 87.999..., so %.0f printed "spine=79deg acute=87deg" -- which reads as
     * the knob having failed to apply, when the derived skip proves it did. */
    TD5_LOG_I(LOG_TAG, "trackgen: heading budget spine=%ddeg acute=%ddeg "
              "-> adjacent_skip=%d",
              (int)(TD5_TG_HEADING_LIMIT * 180.0 / TD5_TG_PI + 0.5),
              (int)(acute_limit * 180.0 / TD5_TG_PI + 0.5), skip);

    if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;

    /* Lead-in: straight road covering the whole starting grid plus the run to
     * the first corner. Must be at least TD5_TG_GRID_SPAN + the grid's own
     * stagger depth, since the grid places cars BEHIND the start line. */
    {
        int i;
        for (i = 0; i < TD5_TG_GRID_SPAN + 16 && nl->count < want_nodes; i++) {
            x += sin(heading) * span_len;
            z += cos(heading) * span_len;
            if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;
        }
    }

    /* Section attempts before we give up and end the track short. A section is
     * retried (with fresh random parameters) when it would overlap earlier
     * road; late attempts are forced straight, which is the most likely shape
     * to escape a cul-de-sac the walk has painted itself into. */
    int attempts = 0;

    while (nl->count < want_nodes) {
        TD5_TrackGenSection sec = tg_pick_section(spec);
        int    len_spans;
        double target_width = base_width;
        double radius = 0.0;
        int    dir = (tg_rand() & 1) ? 1 : -1;
        /* Rollback point, so a rejected section leaves no trace. */
        const int    save_count   = nl->count;
        const double save_x       = x;
        const double save_z       = z;
        const double save_heading = heading;
        const double save_width   = width;
        int rejected = 0;

        if (attempts >= 12) sec = TD5_TG_STRAIGHT;   /* try to escape */

        switch (sec) {
            case TD5_TG_STRAIGHT:
                len_spans = tg_range(6, 24);
                break;

            case TD5_TG_CURVE:
                len_spans = tg_range(8, 26);
                /* Sweeping: comfortably above the safety floor. */
                radius = 12000.0 + tg_frand() * 28000.0;
                break;

            case TD5_TG_ACUTE:
                len_spans = tg_range(4, 12);
                /* Tight: sit just above the curvature-safety floor for the
                 * CURRENT width, so a hairpin never self-intersects the road
                 * surface. */
                radius = (width * 0.5) * (spec->curve_safety_x100 / 100.0)
                       * (1.0 + tg_frand() * 0.6);
                break;

            case TD5_TG_DUAL_LANE:
            default: {
                int extra = tg_range(2, 4);   /* +2..+4 lanes */
                int lanes = spec->lanes + extra;
                if (lanes > 12) lanes = 12;
                target_width = (double)lanes * (double)spec->lane_width;
                len_spans = tg_range(10, 28);
                /* Gentle drift so a wide stretch is not a dead-straight slab. */
                if (tg_rand() & 1)
                    radius = 26000.0 + tg_frand() * 30000.0;
                break;
            }
        }

        /* ACUTE sections may swing to the sharper budget; everything else
         * stays on the spine budget. The forced-straight escape above sets
         * sec = STRAIGHT first, so it correctly uses the spine budget here. */
        const double heading_limit = (sec == TD5_TG_ACUTE)
                                   ? acute_limit : TD5_TG_HEADING_LIMIT;

        {
            int i;
            for (i = 0; i < len_spans && nl->count < want_nodes; i++) {
                int lanes_here;

                /* Ramp width toward this section's target. */
                if (width < target_width) {
                    width += width_ramp;
                    if (width > target_width) width = target_width;
                } else if (width > target_width) {
                    width -= width_ramp;
                    if (width < target_width) width = target_width;
                }

                /* Curvature safety is width-dependent, so re-check every span:
                 * a bend that was legal at 4 lanes can be illegal once a
                 * dual-lane taper has widened the road under it. */
                if (radius > 0.0) {
                    double floor_r = (width * 0.5) * (spec->curve_safety_x100 / 100.0);
                    double r = radius < floor_r ? floor_r : radius;
                    heading += (double)dir * (span_len / r);
                    /* Keep the walk non-trapping (see TD5_TG_HEADING_LIMIT).
                     * On hitting the limit, reverse the turn so the road peels
                     * back off the boundary instead of grinding along it. */
                    if (heading > TD5_TG_AXIS_HEADING + heading_limit) {
                        heading = TD5_TG_AXIS_HEADING + heading_limit;
                        dir = -1;
                    } else if (heading < TD5_TG_AXIS_HEADING - heading_limit) {
                        heading = TD5_TG_AXIS_HEADING - heading_limit;
                        dir = 1;
                    }
                }

                x += sin(heading) * span_len;
                z += cos(heading) * span_len;

                /* Lane COUNT is constant for the whole track; only the WIDTH
                 * varies. Two reasons: (a) consecutive spans must SHARE a
                 * vertex row (see tg_emit_strip) and a shared row has one
                 * point count; (b) shipped tracks only ever use 2-4 lanes, so
                 * the rail LUTs and suspension paths are only exercised there.
                 * A "dual-lane" section is therefore a visibly WIDER road, not
                 * a different subdivision -- which is the visual intent anyway,
                 * since the lane count is really a surface-grid stride. */
                lanes_here = spec->lanes;

                /* Would this node put road on top of earlier road? */
                if (tg_too_close(nl, x, z, width, (double)spec->lane_width,
                                 skip)) {
                    rejected = 1;
                    break;
                }

                if (!tg_nodes_push(nl, x, z, width, lanes_here)) return 0;
            }
        }

        if (rejected) {
            /* Roll the whole section back and try a different one. */
            nl->count = save_count;
            x         = save_x;
            z         = save_z;
            heading   = save_heading;
            width     = save_width;
            attempts++;
            if (attempts >= 24) {
                TD5_LOG_W(LOG_TAG, "trackgen: boxed in after %d spans; ending "
                          "track early (no non-overlapping continuation found)",
                          nl->count - 1);
                break;
            }
            continue;
        }

        attempts = 0;
        section_tally[sec]++;
    }

    /* Unit tangents by central difference (endpoints one-sided). */
    {
        int i;
        for (i = 0; i < nl->count; i++) {
            int a = (i > 0) ? i - 1 : i;
            int b = (i < nl->count - 1) ? i + 1 : i;
            double dx = nl->v[b].x - nl->v[a].x;
            double dz = nl->v[b].z - nl->v[a].z;
            double len = sqrt(dx * dx + dz * dz);
            if (len < 1e-6) { dx = 0.0; dz = 1.0; len = 1.0; }
            nl->v[i].tx = dx / len;
            nl->v[i].tz = dz / len;
        }
    }
    return 1;
}

/* ===================== DELIBERATE BRIDGES =====================
 * Bridges are PLACED, not detected. The organic lift test alone can never
 * produce one: the elevation profile is 2..6 sine waves spread over ~1800
 * spans, so its wavelength is 300..900 spans and across the +/-8 span window
 * the local terrain test uses it is essentially a straight line. Measured
 * local convexity peaks near 160 -- against a 900 threshold that exists
 * because the deck is 780 tall. So the road never rises above its own
 * surroundings fast enough to be "on a bridge", and after the global-minimum
 * bug was fixed no bridge emitted at all.
 *
 * Fix: choose bridge RUNS the same stateless way tunnels choose theirs (hash
 * of si/RUN, no generator state), then drive the elevation into a hump over
 * each run so the road genuinely climbs and the deck has real clearance.
 *
 * The hump is a RAISED COSINE: zero value AND zero slope at both ends, so it
 * splices into the sine profile without a kink that would read as a ramp.
 *
 * Height is bounded by the grade cap, and that bound is the whole reason for
 * these numbers. Peak slope of the hump is H*PI/RUN per span, and
 * tg_apply_elevation rescales the ENTIRE profile if any span exceeds
 * max_grade (0.120 => 180 units per 1500-unit span). A hump too steep would
 * therefore flatten the whole track to fix itself. With RUN 24, H 1300 gives
 * a peak slope of ~170/span (grade 0.113), just inside the cap.
 *
 * That also happens to clear the lift threshold -- at RUN 24 the +/-8 window
 * sits at 0.25H either side, so lift at the crown is 0.75H = 975 > 900 -- but
 * emission deliberately does NOT depend on that. The RANGE decides, exactly
 * as it does for tunnels, so a future grade or amplitude tweak cannot silently
 * delete every bridge again. */
#define TD5_TG_BRIDGE_RUN     24       /* spans per deliberate bridge */
#define TD5_TG_BRIDGE_HEIGHT  1300.0   /* crown lift; bounded by max_grade */

static int tg_bridges_enabled(void)
{
    /* Default OFF: unlike tunnels and guardrails this moves the STRIP, which
     * is the surface the car actually drives on, so it can affect climb, AI
     * pacing and crest jumps. Opt in with TD5RE_AUTOTRACK_BRIDGES=1 until a
     * frame plus the self-test matrix confirm it. */
    return td5_env_flag_off("TD5RE_AUTOTRACK_BRIDGES");
}

/* Is span si inside a deliberately-placed bridge run? Stateless and derived
 * only from si, so the generator, the emitter and the guardrail gate all agree
 * without passing anything around. */
static int tg_span_in_bridge_run(int si)
{
    unsigned int h;
    if (!tg_bridges_enabled()) return 0;
    if (si <= TD5_TG_GRID_SPAN + 40) return 0;   /* not right off the grid */
    h = (unsigned)(si / TD5_TG_BRIDGE_RUN) * 2654435761u;
    return ((h >> 29) == 0);                     /* ~1 run in 8 */
}

/* Two summed sines, a raised-cosine hump over each deliberate bridge run, then
 * a global rescale so no span exceeds MAX_GRADE.
 * Mirrors apply_road_elevation() in td5_trackgen.py, plus the bridge humps. */
static void tg_apply_elevation(const TD5_TrackGenSpec *spec, TG_NodeList *nl)
{
    const double max_grade = spec->max_grade_x1000 / 1000.0;
    double amp = (double)spec->elevation_amplitude;
    double ph1, ph2, worst = 0.0;
    int waves, i;

    if (amp <= 0.0 || nl->count < 3) return;

    waves = tg_range(2, 6);
    ph1 = tg_frand() * 2.0 * TD5_TG_PI;
    ph2 = tg_frand() * 2.0 * TD5_TG_PI;

    for (i = 0; i < nl->count; i++) {
        double f = (double)i / (double)(nl->count - 1);
        nl->v[i].y = amp * (0.6 * sin(2.0 * TD5_TG_PI * waves * f + ph1)
                          + 0.4 * sin(4.0 * TD5_TG_PI * waves * f + ph2));
    }

    /* Deck per deliberate bridge run: flatten the run to its own chord, then
     * add a raised-cosine hump on top.
     *
     * BOTH halves are needed, and the first one is not cosmetic. Hump slope
     * SUPERPOSES on the sine profile's slope. Adding a 0.113-grade hump to a
     * profile already near the 0.120 cap pushed the worst span to 0.169, and
     * tg_apply_elevation's rescale is GLOBAL -- it then multiplied the whole
     * track by 0.708, so switching bridges on silently flattened every hill on
     * the map by 29%. Measured, not hypothetical.
     *
     * Flattening the run to its chord removes the base profile's local slope
     * from the grade budget (and is what a deck looks like anyway -- a bridge
     * spans terrain, it does not undulate over it). The remaining budget is
     * then handed to the hump, whose height is clamped to fit. So no bridge can
     * ever trigger the global rescale, and terrain outside the runs is
     * untouched. */
    {
        int runs = 0, clamped = 0, r0;
        double lowest = TD5_TG_BRIDGE_HEIGHT;
        for (r0 = 0; r0 < nl->count; r0 += TD5_TG_BRIDGE_RUN) {
            int s0 = r0, s1 = r0 + TD5_TG_BRIDGE_RUN - 1, k;
            double yb0, yb1, chord, allowed, hrun;

            if (s1 > nl->count - 1) s1 = nl->count - 1;
            if (s1 <= s0) continue;
            /* One hash per run, so the crown is a fair representative. */
            if (!tg_span_in_bridge_run(s0 + TD5_TG_BRIDGE_RUN / 2)) continue;

            yb0 = nl->v[s0].y;
            yb1 = nl->v[s1].y;
            chord = (yb1 - yb0) / (double)(s1 - s0);
            for (k = s0; k <= s1; k++) {
                double u = (double)(k - s0) / (double)(s1 - s0);
                nl->v[k].y = yb0 + (yb1 - yb0) * u;
            }

            /* Peak slope of a raised cosine of height H over RUN spans is
             * H*PI/RUN, so invert that against the leftover budget. */
            if (max_grade <= 0.0) {
                hrun = TD5_TG_BRIDGE_HEIGHT;      /* no cap configured */
            } else {
                allowed = max_grade * (double)spec->span_length - fabs(chord);
                if (allowed < 0.0) allowed = 0.0;
                hrun = allowed * (double)TD5_TG_BRIDGE_RUN / TD5_TG_PI;
                if (hrun > TD5_TG_BRIDGE_HEIGHT) hrun = TD5_TG_BRIDGE_HEIGHT;
            }

            for (k = s0; k <= s1; k++) {
                double t = ((double)(k - s0) + 0.5) / (double)TD5_TG_BRIDGE_RUN;
                nl->v[k].y += hrun * 0.5 * (1.0 - cos(2.0 * TD5_TG_PI * t));
            }
            runs++;
            if (hrun < TD5_TG_BRIDGE_HEIGHT - 1.0) clamped++;
            if (hrun < lowest) lowest = hrun;
        }
        if (runs)
            TD5_LOG_I(LOG_TAG, "trackgen: %d deliberate bridge run(s) of %d "
                      "spans, crown +%.0f (%d grade-clamped, lowest +%.0f)",
                      runs, TD5_TG_BRIDGE_RUN, TD5_TG_BRIDGE_HEIGHT,
                      clamped, lowest);
    }

    /* Anchor the profile to y=0 at the start line. The grid spawn places cars
     * at y~0 rather than sampling the road, so a track whose span 0 sits at
     * (say) -2040 drops every car into a ~2000-unit free-fall at the green
     * light -- observed as "WHEELS: ----" and the racers never touching down. */
    {
        double y0 = nl->v[0].y;
        for (i = 0; i < nl->count; i++) nl->v[i].y -= y0;
    }

    for (i = 1; i < nl->count; i++) {
        double dy = nl->v[i].y - nl->v[i - 1].y;
        double g  = fabs(dy) / (double)spec->span_length;
        if (g > worst) worst = g;
    }
    if (max_grade > 0.0 && worst > max_grade) {
        double k = max_grade / worst;
        for (i = 0; i < nl->count; i++) nl->v[i].y *= k;
        TD5_LOG_I(LOG_TAG, "trackgen: elevation rescaled by %.3f (grade %.3f -> %.3f)",
                  k, worst, max_grade);
    }
}

/* ------------------------------------------------------ byte emitters ----- */
typedef struct {
    unsigned char *b;
    size_t         len;
    size_t         cap;
    int            oom;
} TG_Buf;

static void tg_buf_free(TG_Buf *buf)
{
    free(buf->b);
    buf->b = NULL;
    buf->len = buf->cap = 0;
}

static int tg_buf_need(TG_Buf *buf, size_t extra)
{
    if (buf->oom) return 0;
    if (buf->len + extra <= buf->cap) return 1;
    {
        size_t cap = buf->cap ? buf->cap : 4096;
        unsigned char *nb;
        while (cap < buf->len + extra) cap *= 2;
        nb = (unsigned char *)realloc(buf->b, cap);
        if (!nb) { buf->oom = 1; return 0; }
        buf->b = nb;
        buf->cap = cap;
    }
    return 1;
}

static void tg_put_u8(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 1)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
}

static void tg_put_u16(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 2)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
}

static void tg_put_u32(TG_Buf *buf, unsigned int v)
{
    if (!tg_buf_need(buf, 4)) return;
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 8) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 16) & 0xFF);
    buf->b[buf->len++] = (unsigned char)((v >> 24) & 0xFF);
}

static void tg_put_i32(TG_Buf *buf, int v)
{
    tg_put_u32(buf, (unsigned int)v);
}

static void tg_put_zeros(TG_Buf *buf, size_t n)
{
    if (!tg_buf_need(buf, n)) return;
    memset(buf->b + buf->len, 0, n);
    buf->len += n;
}

/* Copy a file verbatim. Used for the sky: the renderer wants a 256x256 RGBA
 * FORWSKY.png in the level directory, and reusing a shipped panorama is both
 * guaranteed-valid and better looking than anything this generator could
 * synthesise. Forward-only tracks need no BACKSKY -- the loader falls back to
 * FORWSKY when it is absent (td5_game.c:4644-4648). */
static int tg_copy_file(const char *src, const char *dst)
{
    FILE *fi, *fo;
    char buf[16384];
    size_t n;
    int ok = 1;

    fi = fopen(src, "rb");
    if (!fi) return 0;
    fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) { ok = 0; break; }
    }
    fclose(fi);
    fclose(fo);
    return ok;
}

/* Install a sky by borrowing one from a shipped level, chosen from the seed so
 * different generated tracks get different skies. Candidates are probed in
 * order because not every level ships one. Non-fatal: without it the race just
 * renders against the flat clear colour, which is what happened before. */
static void tg_install_sky(const char *dir, unsigned int seed)
{
    static const int k_sky_levels[] = { 1, 2, 3, 5, 8, 13, 21, 29 };
    const int n = (int)(sizeof(k_sky_levels) / sizeof(k_sky_levels[0]));
    char src[256], dst[320];
    int i;

    for (i = 0; i < n; i++) {
        int lvl = k_sky_levels[(seed / 7u + (unsigned)i) % (unsigned)n];
        snprintf(src, sizeof(src),
                 "re/assets/levels/level%03d/FORWSKY.png", lvl);
        snprintf(dst, sizeof(dst), "%s/FORWSKY.png", dir);
        if (tg_copy_file(src, dst)) {
            TD5_LOG_I(LOG_TAG, "trackgen: sky from level%03d -> %s", lvl, dst);
            return;
        }
    }
    TD5_LOG_W(LOG_TAG, "trackgen: no shipped FORWSKY.png found to borrow; "
              "race will render against the flat clear colour");
}

static int tg_write_file(const char *dir, const char *name,
                         const unsigned char *data, size_t len)
{
    char path[320];
    FILE *f;
    size_t wrote;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        TD5_LOG_E(LOG_TAG, "trackgen: cannot open %s for writing", path);
        return 0;
    }
    wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) {
        TD5_LOG_E(LOG_TAG, "trackgen: short write on %s (%zu/%zu)",
                  path, wrote, len);
        return 0;
    }
    TD5_LOG_I(LOG_TAG, "trackgen: wrote %s (%zu bytes)", path, len);
    return 1;
}

/* Round-to-nearest for signed doubles (lrint is not uniformly available). */
static int tg_round(double v)
{
    return (int)(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

/* ------------------------------------------------------- STRIP.DAT ------- */
/*
 * Layout (confirmed against td5_track.c's loader and td5_assetsrc.c's encoder):
 *   0x00 u32 span table byte offset      (= TD5_TG_SPAN_OFFSET, 216)
 *   0x04 u32 ring length                 (main-road span count)
 *   0x08 u32 vertex table byte offset
 *   0x0C u32 vertex count
 *   0x10 u32 total span count
 *   0x14 u32 branch jump-entry count     (0 -- no branches in Phase 1)
 *   0x18..0xD7  jump records / zero pad  (TD5_TG_PRE_SPAN_BYTES total)
 *   then span records (24 B each), then vertices (6 B each).
 *
 * Span record: type u8 | surface u8 | lane bitmask u8 | lanes|height u8 |
 *              left_vtx u16 | right_vtx u16 | link_next i16 | link_prev i16 |
 *              origin_x i32 | origin_y i32 | origin_z i32
 * Vertex: i16 x,y,z -- LOCAL offsets from that span's origin.
 *
 * Each span emits its own near row then far row (rows are duplicated at seams,
 * exactly as the Python emitter does -- the loader tolerates it).
 */
/* ===================== BRANCHES =====================
 * Full spec and provenance: docs/plans/AUTOTRACK_BRANCHES.md.
 *
 * DEFAULT OFF (TD5RE_AUTOTRACK_BRANCHES=1). A branch changes strip TOPOLOGY,
 * not just geometry, so a mistake corrupts the whole track rather than looking
 * wrong. Structurally complete but NOT verified in game.
 *
 * Design note: the main road keeps its verified shared-row emission untouched.
 * Junction and corridor spans instead get DEDICATED rows appended afterwards,
 * and their records are patched in place. That is also what native TD5 does --
 * level014's fork span owns its rows and the corridor start duplicates them --
 * so per-span lane counts are legal without disturbing the shared-row blocks
 * that fixed seam contact.
 *
 * Layout produced (ring = nspans = main road only):
 *   0..nspans-1     main road; [F-W..F] widened to main+branch, F is type 8
 *   nspans          PAD span -- exists solely so the corridor can start at
 *                   ring+1, because td5_track_branch_to_main_span REJECTS
 *                   span <= ring (td5_track.c:8185). td5_trackgen.py gets this
 *                   wrong (lo == ring) and must not be copied.
 *   nspans+1..      corridor: type 9, type 1 interior, type 10 linking to R
 *   jump record     (lo=nspans+1, hi=last, base=F+1) so main = span - lo + base
 */
#define TD5_TG_BRANCH_FORK_SPAN  600   /* fixed, so a test can drive to it */
#define TD5_TG_BRANCH_LEN         40   /* corridor spans */
#define TD5_TG_BRANCH_WIDEN        6   /* approach spans widened before F */

static int tg_branches_enabled(void)
{
    return td5_env_flag_off("TD5RE_AUTOTRACK_BRANCHES");
}

/* Append one vertex row of (lanes+1) points for node n, relative to (ox,oy,oz),
 * laterally shifted by `shift` (world units, +ve = left of travel) and using
 * `width`. Returns the row's first vertex index. */
static int tg_append_row(TG_Buf *verts, int *vtx_count, const TG_Node *n,
                         int lanes, double width, double shift,
                         int ox, int oy, int oz)
{
    const double lx = n->tz, lz = -n->tx;
    const int first = *vtx_count;
    int j;
    for (j = 0; j <= lanes; j++) {
        double t = shift + (width * 0.5) - (width * (double)j / (double)lanes);
        int dx = tg_round(n->x + lx * t) - ox;
        int dy = tg_round(n->y) - oy;
        int dz = tg_round(n->z + lz * t) - oz;
        tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
        tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
    }
    *vtx_count += lanes + 1;
    return first;
}

/* Overwrite fields of an already-emitted span record in place. */
static void tg_patch_span(TG_Buf *spans, int si, int type, int lanes,
                          int lvi, int rvi, int link_next, int link_prev,
                          int ox, int oy, int oz)
{
    unsigned char *r = spans->b + (size_t)si * 24;
    r[0] = (unsigned char)type;
    r[3] = (unsigned char)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F));
    r[4] = (unsigned char)(lvi & 0xFF);       r[5] = (unsigned char)(lvi >> 8);
    r[6] = (unsigned char)(rvi & 0xFF);       r[7] = (unsigned char)(rvi >> 8);
    r[8] = (unsigned char)(link_next & 0xFF); r[9] = (unsigned char)((link_next >> 8) & 0xFF);
    r[10]= (unsigned char)(link_prev & 0xFF); r[11]= (unsigned char)((link_prev >> 8) & 0xFF);
    {   /* origin: these spans own their rows, so they own their origin too */
        int i, v[3];
        v[0] = ox; v[1] = oy; v[2] = oz;
        for (i = 0; i < 3; i++) {
            unsigned int u = (unsigned int)v[i];
            r[12 + i*4 + 0] = (unsigned char)(u & 0xFF);
            r[12 + i*4 + 1] = (unsigned char)((u >> 8) & 0xFF);
            r[12 + i*4 + 2] = (unsigned char)((u >> 16) & 0xFF);
            r[12 + i*4 + 3] = (unsigned char)((u >> 24) & 0xFF);
        }
    }
}

/* Append a whole span record (used for the pad span and the corridor). */
static void tg_append_span(TG_Buf *spans, int type, int lanes,
                           int lvi, int rvi, int link_next, int link_prev,
                           int ox, int oy, int oz)
{
    tg_put_u8 (spans, (unsigned)type);
    tg_put_u8 (spans, TD5_TG_SURFACE_ATTR);
    tg_put_u8 (spans, (unsigned)(1 | (1 << (lanes - 1))));
    tg_put_u8 (spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4) | (lanes & 0x0F)));
    tg_put_u16(spans, (unsigned)lvi);
    tg_put_u16(spans, (unsigned)rvi);
    tg_put_u16(spans, (unsigned)(link_next & 0xFFFF));
    tg_put_u16(spans, (unsigned)(link_prev & 0xFFFF));
    tg_put_i32(spans, ox);
    tg_put_i32(spans, oy);
    tg_put_i32(spans, oz);
}

/* NATIVE-FAITHFUL FORK (matches level014). A real TD5 fork does NOT widen the
 * road: a constant-width road SPLITS DOWN THE MIDDLE into two half-width
 * carriageways, which run apart and then merge back into a full-width road at a
 * type-11 rejoin (level014: 6 lanes -> 3 main + 3 branch; rejoin 4+4 -> 8).
 *
 * So here the `lanes`-wide road splits into a MAIN (left) half on sub-lanes
 * [0, lanes/2) and a BRANCH (right) half on [lanes/2, lanes). Each half is
 * lane_count/2 wide, centred a quarter-width off the road centreline:
 *   main centre   = +width/4  (left of travel)
 *   branch centre = -width/4  (right of travel), plus an outward bow
 * The fork span itself and the rejoin span stay FULL width (they are where the
 * two halves share the road); everything between is the two half carriageways.
 *
 * Because the two halves never occupy the same ground (they are opposite halves
 * that only bow further apart), the collision walker cannot flicker between
 * them, and because the rejoin merges back to the road's OWN width there is no
 * forward lane-drop to strand -- both problems the widen-and-converge version
 * had. */
#define TD5_TG_BRANCH_BOW  1.20   /* extra outward sag of the branch, x width */

/* Lateral centre of the MAIN (left) half carriageway -- constant. */
#define TD5_TG_MAIN_SHIFT(w)   ((w) * 0.25)

/* Lateral centre of the BRANCH (right) half carriageway at corridor step k:
 * the right-half centre (-width/4) plus an outward bow that is 0 at both ends
 * (so it lines back up with the road halves at the fork and the rejoin) and
 * peaks in the middle. */
static double tg_branch_shift(int k, double width)
{
    double f   = (double)k / (double)TD5_TG_BRANCH_LEN;   /* 0 .. 1 */
    double bow = sin(f * TD5_TG_PI);                      /* 0 -> 1 -> 0 */
    return -width * 0.25 - width * TD5_TG_BRANCH_BOW * bow;
}

/* ===================== [S1] RANGE EMITTER =====================
 * Emit spans [first_span, first_span+span_count) and their vertex rows into
 * CALLER-OWNED buffers, with vertex indices continuing from *vtx_count.
 *
 * This exists for Phase 2 streaming (docs/plans/AUTOTRACK_STREAMING.md): a
 * rolling ring buffer has to rewrite a REGION of the track, not rebuild the
 * whole thing, so the emitter has to be addressable by range.
 *
 * CONTRACT: first_span MUST be block-aligned (first_span % block == 0), and
 * span_count SHOULD be a multiple of block except for the final partial one.
 * Origin blocks share an origin and share rows across the block, so a range
 * starting mid-block would produce a different origin for those spans and
 * different vertex indices -- silently different geometry, not an error. The
 * streaming design already advances the write cursor in block units for this
 * reason. Asserted below rather than assumed.
 */
static int tg_emit_span_range(const TG_NodeList *nl, int first_span,
                              int span_count, int lanes, int block,
                              TG_Buf *spans, TG_Buf *verts, int *vtx_count)
{
    const int row_pts = lanes + 1;
    const int range_end = first_span + span_count;
    int s0, ok = 1;

    if (block <= 0 || span_count <= 0) return 0;
    if (first_span % block != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range first_span=%d is not aligned "
                  "to the %d-span origin block; refusing (would silently "
                  "change geometry)", first_span, block);
        return 0;
    }
    if (range_end > nl->count - 1) {
        TD5_LOG_E(LOG_TAG, "trackgen: emit range [%d,%d) exceeds %d spans",
                  first_span, range_end, nl->count - 1);
        return 0;
    }

    for (s0 = first_span; s0 < range_end; s0 += block) {
        const int ns  = (s0 + block <= range_end) ? block : (range_end - s0);
        const int ox  = tg_round(nl->v[s0].x);
        const int oy  = tg_round(nl->v[s0].y);
        const int oz  = tg_round(nl->v[s0].z);
        const int base = *vtx_count;
        int k;

        if (*vtx_count + (ns + 1) * row_pts > TD5_TG_MAX_VERTICES) {
            TD5_LOG_W(LOG_TAG, "trackgen: vertex ceiling hit at span %d "
                      "(%d verts); truncating", s0, *vtx_count);
            break;
        }

        /* ns+1 rows, one per node s0..s0+ns inclusive, all relative to this
         * block's origin. The shared row at a block seam is re-emitted under
         * the new origin, exactly as shipped tracks do. */
        for (k = 0; k <= ns; k++) {
            const TG_Node *n = &nl->v[s0 + k];
            const double lx = n->tz, lz = -n->tx;
            int j;
            for (j = 0; j < row_pts; j++) {
                double t  = (n->width * 0.5)
                          - (n->width * (double)j / (double)lanes);
                int dx = tg_round(n->x + lx * t) - ox;
                int dy = tg_round(n->y) - oy;
                int dz = tg_round(n->z + lz * t) - oz;
                if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767 ||
                    dz < -32768 || dz > 32767) {
                    TD5_LOG_E(LOG_TAG, "trackgen: vertex offset out of int16 "
                              "range in block at span %d (%d,%d,%d) -- reduce "
                              "the origin block size", s0, dx, dy, dz);
                    ok = 0;
                }
                tg_put_u16(verts, (unsigned)(dx & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dy & 0xFFFF));
                tg_put_u16(verts, (unsigned)(dz & 0xFFFF));
            }
            *vtx_count += row_pts;
        }

        for (k = 0; k < ns; k++) {
            tg_put_u8 (spans, 1);                        /* span_type QUAD_A */
            tg_put_u8 (spans, TD5_TG_SURFACE_ATTR);
            /* Lane bitmask 0 = every lane is the low-nibble surface (dry asphalt,
         * full grip). The old `1 | (1<<(lanes-1))` marked the OUTER lanes, which
         * surface_type_for_span_lane turns into the 0x10 "alternate/off-road"
         * surface -- td5_track_surface_is_slow returns 1 for anything with 0x10
         * set, so those lanes silently SLOWED the car while textured identically
         * to the fast lanes (reported as "lanes that make the car slower look the
         * same as the road"). A generated arcade road is uniformly drivable. */
        tg_put_u8 (spans, 0);
            tg_put_u8 (spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4)
                                         | (lanes & 0x0F)));
            tg_put_u16(spans, (unsigned)(base + k * row_pts));
            tg_put_u16(spans, (unsigned)(base + (k + 1) * row_pts));
            tg_put_u16(spans, 0xFFFF);                   /* link_next = -1 */
            tg_put_u16(spans, 0xFFFF);                   /* link_prev = -1 */
            tg_put_i32(spans, ox);
            tg_put_i32(spans, oy);
            tg_put_i32(spans, oz);
        }
    }
    return ok && !spans->oom && !verts->oom;
}

/* [S1 GATE] Prove the range emitter is composable: emitting the track in
 * block-aligned CHUNKS must be byte-identical to emitting it in one call. If
 * that does not hold, streaming would silently produce different geometry from
 * the same seed. Runs only when TD5RE_AUTOTRACK_SELFCHECK=1. */
static void tg_selfcheck_ranges(const TG_NodeList *nl, int lanes, int block)
{
    const int nspans = nl->count - 1;
    TG_Buf one_s, one_v, many_s, many_v;
    int vc_one = 0, vc_many = 0, s0, ok = 1;
    int chunk = block * 5;      /* several blocks per chunk, still aligned */

    memset(&one_s, 0, sizeof(one_s));   memset(&one_v, 0, sizeof(one_v));
    memset(&many_s, 0, sizeof(many_s)); memset(&many_v, 0, sizeof(many_v));

    if (!tg_emit_span_range(nl, 0, nspans, lanes, block, &one_s, &one_v,
                            &vc_one))
        ok = 0;

    for (s0 = 0; ok && s0 < nspans; s0 += chunk) {
        int n = (s0 + chunk <= nspans) ? chunk : (nspans - s0);
        if (!tg_emit_span_range(nl, s0, n, lanes, block, &many_s, &many_v,
                                &vc_many))
            ok = 0;
    }

    if (!ok) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: range emit FAILED");
    } else if (one_s.len != many_s.len || one_v.len != many_v.len ||
               vc_one != vc_many) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL size (spans %zu vs %zu, "
                  "verts %zu vs %zu, vtx %d vs %d)", one_s.len, many_s.len,
                  one_v.len, many_v.len, vc_one, vc_many);
    } else if (memcmp(one_s.b, many_s.b, one_s.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL span bytes differ");
    } else if (memcmp(one_v.b, many_v.b, one_v.len) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: FAIL vertex bytes differ");
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: PASS -- %d spans emitted in "
                  "%d-span chunks is byte-identical to one shot (%zu span B, "
                  "%zu vtx B, %d verts)", nspans, chunk, one_s.len, one_v.len,
                  vc_one);
    }

    tg_buf_free(&one_s);  tg_buf_free(&one_v);
    tg_buf_free(&many_s); tg_buf_free(&many_v);
}

static int tg_emit_strip(const TG_NodeList *nl, TG_Buf *out, int *out_spans)
{
    const int nspans = nl->count - 1;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 1;

    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    /* Lane count is uniform (see tg_build_centerline), so every row has the
     * same point count and consecutive spans can SHARE a row: span i spans
     * rows i and i+1. Shipped tracks do exactly this (level001: span0
     * lvi=0 rvi=5, span1 lvi=5 rvi=10). Emitting a private duplicated row
     * pair per span is geometrically identical but breaks span-to-span
     * adjacency, which resolve_neighbor (td5_track.c:4013) detects by vertex
     * INDEX -- contact then fails at every seam and the car sinks through the
     * road (measured: wheel_mask=0 for 343/385 ticks, even dead flat). */
    const int lanes   = nl->v[0].lanes;
    /* Reset per-call: a failed or branch-less build must not leave a stale
     * jump record from a previous generation in the header. */
    s_jump_lo = s_jump_hi = s_jump_base = s_jump_have = 0;
    s_ring_len = 0;
    /* Spans per shared origin. Tunable so the effect of origin granularity on
     * ground contact is measurable: the ground probe appears to read a span's
     * origin_y (a track starting at y=-2040 probed -522240 = -2040*256), and a
     * shared origin_y makes the collision ground a flat step at the block base
     * while the visible road ramps away from it on hills. */
    const int block = td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                  TD5_TG_ORIGIN_BLOCK, 1, 20);

    /* ORIGIN BLOCKS. The loader resolves BOTH of a span's vertex rows against
     * THAT span's origin, so two spans can only share a row if they share an
     * origin. Hence origins are per-BLOCK, constant across the block, with the
     * shared row duplicated at block seams -- which is exactly what shipped
     * tracks do (level001 span0 lvi=0 rvi=5, span1 lvi=5 rvi=10).
     *
     * Storing one origin per span while sharing rows displaces every span's FAR
     * edge by one span step, skewing the road surface: contact then flickers
     * between all-four-wheels and none, and the car gets flung down-track
     * (measured: wall_clear median -583186 while driving, though a perfect 2250
     * at rest -- the giveaway that only the far row was wrong).
     *
     * Block length is bounded by the int16 vertex offset: TD5_TG_ORIGIN_BLOCK
     * spans of span_length, plus half the widest road, must stay under 32767. */
    /* [S1] Whole track = one block-aligned range. The emitter below can write
     * any block-aligned range into caller buffers, which is what Phase 2
     * streaming needs to rewrite a region in place. */
    if (!tg_emit_span_range(nl, 0, nspans, lanes, block, &spans, &verts,
                            &vtx_count))
        ok = 0;

    /* ---- BRANCH (opt-in) ---- */
    {
        const int emitted_main = (int)(spans.len / 24);
        int jump_lo = 0, jump_hi = 0, jump_base = 0, have_jump = 0;

        if (ok && tg_branches_enabled() &&
            emitted_main > TD5_TG_BRANCH_FORK_SPAN + TD5_TG_BRANCH_LEN + 8) {
            const int F     = TD5_TG_BRANCH_FORK_SPAN;
            const int ring  = emitted_main;
            const int b0    = ring + 1;               /* NOT ring: see spec */
            const int LEN   = TD5_TG_BRANCH_LEN;
            const int main_half = lanes / 2;          /* main (left) carriageway */
            const int br_lanes  = lanes - main_half;  /* branch (right) carriageway */
            const int R     = F + 1 + LEN;            /* rejoin span (main ring) */
            int k;

            /* 1. FORK span F: stays FULL width and splits down the middle. Its
             *    own dedicated full-width rows, type 8, link_next -> corridor.
             *    sub_lane < main_half -> main (span F+1); else -> branch (b0). */
            {
                const TG_Node *a = &nl->v[F], *b = &nl->v[F + 1];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                        a->width, 0.0, ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                        b->width, 0.0, ox, oy, oz);
                tg_patch_span(&spans, F, 8, lanes, lvi, rvi, b0, -1, ox, oy, oz);
            }

            /* 2. MAIN carriageway [F+1 .. F+LEN]: narrow to the LEFT half. Each
             *    span gets dedicated rows at main_half lanes, centre +width/4. */
            for (k = 1; k <= LEN; k++) {
                const int si = F + k;
                const TG_Node *a = &nl->v[si], *b = &nl->v[si + 1];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, main_half,
                                        a->width * 0.5, TD5_TG_MAIN_SHIFT(a->width),
                                        ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, b, main_half,
                                        b->width * 0.5, TD5_TG_MAIN_SHIFT(b->width),
                                        ox, oy, oz);
                tg_patch_span(&spans, si, 1, main_half, lvi, rvi, -1, -1,
                              ox, oy, oz);
            }

            /* 3. PAD span at index == ring, so the corridor starts at ring+1.
             *    Geometry is irrelevant; it is never linked to. */
            {
                const TG_Node *a = &nl->v[F];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                        a->width * 0.5, 0.0, ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                        a->width * 0.5, 0.0, ox, oy, oz);
                tg_append_span(&spans, 1, br_lanes, lvi, rvi, -1, -1,
                               ox, oy, oz);
            }

            /* 4. BRANCH corridor b0..b0+LEN-1: the RIGHT half, centre -width/4
             *    plus an outward bow, running parallel to main spans F+1..F+LEN.
             *    type 9 start / 1 interior / 10 end (links to the rejoin). */
            for (k = 0; k < LEN; k++) {
                const TG_Node *a = &nl->v[F + 1 + k], *b = &nl->v[F + 2 + k];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                        a->width * 0.5, tg_branch_shift(k, a->width),
                                        ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, b, br_lanes,
                                        b->width * 0.5, tg_branch_shift(k + 1, b->width),
                                        ox, oy, oz);
                int type = (k == 0) ? 9 : ((k == LEN - 1) ? 10 : 1);
                int nxt  = (k == LEN - 1) ? R : -1;
                int prv  = (k == 0) ? F : -1;
                tg_append_span(&spans, type, br_lanes, lvi, rvi, nxt, prv,
                               ox, oy, oz);
            }

            /* 5. REJOIN span R: back to FULL width -- the two half carriageways
             *    merge here. Dedicated full-width rows, type 11 (JUNCTION_BWD),
             *    link_prev -> corridor SENTINEL_END. Main (R-1, main_half) feeds
             *    the LEFT lanes, the branch (br_lanes) feeds the right via the
             *    type-10 forward delta (+= lanes(R) - br_lanes). Because R is the
             *    road's OWN width there is no forward lane-drop after it, so
             *    nothing is stranded -- and because lanes(R) > lanes(R-1) the
             *    reverse split fires too, so the branch is reverse-drivable. */
            {
                const int sentinel_end = ring + LEN; /* pad@ring, corridor ring+1..ring+LEN */
                const TG_Node *a = &nl->v[R], *b = &nl->v[R + 1];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, lanes,
                                        a->width, 0.0, ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, b, lanes,
                                        b->width, 0.0, ox, oy, oz);
                if (R >= 0 && R < ring)
                    tg_patch_span(&spans, R, 11, lanes, lvi, rvi, -1,
                                  sentinel_end, ox, oy, oz);
            }

            jump_lo   = b0;
            jump_hi   = b0 + TD5_TG_BRANCH_LEN - 1;
            jump_base = F + 1;
            have_jump = 1;
            TD5_LOG_I(LOG_TAG, "trackgen: branch fork=%d corridor=%d..%d "
                      "base=%d rejoin=%d(type11) (ring=%d, main=span-%d)",
                      F, jump_lo, jump_hi, jump_base, F + 1 + TD5_TG_BRANCH_LEN,
                      ring, jump_lo - jump_base);
        }

        s_jump_lo = jump_lo; s_jump_hi = jump_hi;
        s_jump_base = jump_base; s_jump_have = have_jump;
        s_ring_len = emitted_main;
    }

    if (spans.oom || verts.oom) {
        TD5_LOG_E(LOG_TAG, "trackgen: out of memory building strip");
        ok = 0;
    }

    if (ok) {
        const int emitted = (int)(spans.len / 24);
        const unsigned int vtx_off =
            (unsigned int)(TD5_TG_SPAN_OFFSET + 24 * emitted);

        tg_put_u32(out, TD5_TG_SPAN_OFFSET);
        /* Ring length = the MAIN ROAD span count. With a branch, `emitted`
         * also counts the pad + corridor spans, which must sit OUTSIDE the
         * ring -- that is what makes branch spans normalize through the jump
         * table. Without a branch the two are identical.
         *
         * Shipped level001 writes span_count-1 but it is a CIRCUIT, where the
         * last span closes onto the first. Copying that made the walker wrap
         * backward off span 0 to the ring end (span_raw oscillated 0 <-> 1798
         * every tick, 952 times in one run). */
        tg_put_u32(out, (unsigned int)(s_ring_len > 0 ? s_ring_len : emitted));
        tg_put_u32(out, vtx_off);
        tg_put_u32(out, (unsigned int)vtx_count);
        tg_put_u32(out, (unsigned int)emitted);   /* total spans */
        /* Jump-entry count at 0x14, then 6-byte records from 0x18 (native TD5
         * offset; 0x20 is the TD6-converted form). */
        tg_put_u32(out, (unsigned int)(s_jump_have ? 1 : 0));
        if (s_jump_have) {
            tg_put_u16(out, (unsigned)s_jump_lo);
            tg_put_u16(out, (unsigned)s_jump_hi);
            tg_put_u16(out, (unsigned)s_jump_base);
            tg_put_zeros(out, TD5_TG_PRE_SPAN_BYTES - 4 - 6);
        } else {
            /* No records. This block must stay exactly TD5_TG_PRE_SPAN_BYTES
             * long -- the loader derives the span count from
             * (vtx_off - span_off)/24. */
            tg_put_zeros(out, TD5_TG_PRE_SPAN_BYTES - 4);
        }
        if (!tg_buf_need(out, spans.len + verts.len)) {
            ok = 0;
        } else {
            memcpy(out->b + out->len, spans.b, spans.len);
            out->len += spans.len;
            memcpy(out->b + out->len, verts.b, verts.len);
            out->len += verts.len;
        }
        if (out->len != (size_t)vtx_off + (size_t)vtx_count * 6) {
            TD5_LOG_E(LOG_TAG, "trackgen: strip size mismatch (%zu vs %u)",
                      out->len, (unsigned)(vtx_off + vtx_count * 6));
            ok = 0;
        }
        *out_spans = emitted;
        TD5_LOG_I(LOG_TAG, "trackgen: strip = %d spans, %d vertices, %zu bytes",
                  emitted, vtx_count, out->len);
    }

    tg_buf_free(&spans);
    tg_buf_free(&verts);
    return ok && !out->oom;
}

/* ---------------------------------------------------- LEFT/RIGHT.TRK ----- */
/* 3 bytes per span, exactly ring-length rows (short tables are what causes the
 * out-of-bounds read documented at td5_track.c:3503).
 *   byte0 = lateral corridor position, 0 = left rail .. 255 = right rail
 *   byte1 = local heading, 128 = straight ahead
 *   byte2 = authored corner-speed cap, 255 = uncapped (curvature governs)
 */
static int tg_emit_routes(const TG_NodeList *nl, int nspans,
                          int lateral, TG_Buf *out)
{
    int i;
    for (i = 0; i < nspans; i++) {
        /* byte1 is the ABSOLUTE 12-bit heading, not a deflection: the engine
         * recovers it as heading = (byte * 0x102C) >> 8 (td5_ai.c:1280, and the
         * same formula in ai_route_heading_for_actor at td5_ai.c:305), so invert
         * exactly that. Yaw convention is forward = (sin h, cos h) --
         * td5_physics.c, cited at td5_ai.c:1306 -- hence atan2(tx, tz).
         *
         * Getting this wrong is not subtle: encoding a deflection here (128 =
         * straight) spawned every car pointing the wrong way, and with
         * auto-throttle they drove backwards off the start line into the void. */
        double h = atan2(nl->v[i].tx, nl->v[i].tz) * 4096.0 / (2.0 * TD5_TG_PI);
        int h12 = tg_round(h) & 0xFFF;
        int hb  = tg_round((double)h12 * 256.0 / 4140.0);   /* 4140 = 0x102C */

        /* byte < 4 is a junction-zone sentinel, not a heading. The axis offset
         * (TD5_TG_AXIS_HEADING) keeps us clear of it; clamp as a backstop. */
        if (hb < 4)   hb = 4;
        if (hb > 253) hb = 253;

        tg_put_u8(out, (unsigned)lateral);
        tg_put_u8(out, (unsigned)hb);
        tg_put_u8(out, 255);
    }
    return !out->oom;
}

/* ------------------------------------------------------ LEVELINF.DAT ----- */
/* 100 bytes. The level loader reads DWORD[0] (1 = circuit, else
 * point-to-point) and keeps the rest as opaque environment config. */
static int tg_emit_levelinf(const TD5_TrackGenSpec *spec, int nspans,
                            TG_Buf *out)
{
    int cp_count = 4, i;
    int cp_span[7];

    for (i = 0; i < 7; i++) cp_span[i] = 0;
    if (nspans < 200) cp_count = 2;
    for (i = 0; i < cp_count; i++)
        cp_span[i] = (int)((long)nspans * (i + 1) / (cp_count + 1));

    tg_put_u32(out, spec->circuit ? 1u : 0u);   /* 0x00 track_type */
    tg_put_u32(out, 1);                         /* 0x04 smoke_enable */
    tg_put_u32(out, (unsigned)cp_count);        /* 0x08 checkpoint_count */
    for (i = 0; i < 7; i++)                     /* 0x0C checkpoint_spans */
        tg_put_u32(out, (unsigned)cp_span[i]);
    tg_put_u32(out, 2);                         /* 0x28 weather_type */
    tg_put_u32(out, 0);                         /* 0x2C density_pair_count */
    tg_put_u32(out, 0);                         /* 0x30 traffic_enable */
    tg_put_zeros(out, 24);                      /* 0x34 density_pairs 12xu16 */
    tg_put_zeros(out, 8);                       /* 0x4C pad */
    /* 0x54 sky_animation_index. Shipped circuits use 36 and point-to-point
     * tracks use -1, but -1 appears to be why a generated track renders
     * against a flat clear colour with no sky at all, so use the circuit index
     * regardless. [UNCERTAIN] Untested when written -- if it does not produce a
     * sky, the other candidate is the missing FORWSKY.png the renderer logs
     * ("sky not found: re/assets/levels/levelNNN/FORWSKY.png"). */
    tg_put_u32(out, 36u);
    tg_put_u32(out, (unsigned)nspans);          /* 0x58 total_span_count */
    tg_put_u32(out, 0);                         /* 0x5C fog_enabled */
    tg_put_u8(out, 0);                          /* 0x60 fog r */
    tg_put_u8(out, 0);                          /* 0x61 fog g */
    tg_put_u8(out, 0);                          /* 0x62 fog b */
    tg_put_u8(out, 0);                          /* 0x63 pad */
    return !out->oom;
}

/* ==========================================================================
 * MODELS.DAT -- road surface mesh
 *
 * Emitting ANY MODELS.DAT switches OFF the procedural ribbon renderer (it is
 * gated on "zero display-list entries", td5_render_mesh.c:2663), so the road
 * surface must come from here instead. Hence TD5RE_AUTOTRACK_SCENERY defaults
 * to OFF: until this is verified, the shipped path stays the ribbon.
 *
 * Byte format is a C port of re/tools/mesh_tool.py (_pack_mesh, build_dat) --
 * the emitter the existing Python-generated levels are validated against --
 * rather than a fresh reading of the parser.
 *
 * Container ("format A strict", td5_track_parser.c:79-119):
 *   u32 entry_count
 *   (u32 block_offset, u32 block_size) * entry_count   -- offset ABSOLUTE
 *   blocks, contiguous; block 0 must start at 4 + count*8 (no padding)
 * Block:
 *   u32 sub_count (1..256)
 *   u32 mesh_off[sub_count]   -- BLOCK-relative; 0 = empty slot
 *   packed mesh records
 * Mesh record is 0x38 bytes, then commands (16 B each), then vertices (44 B).
 * ========================================================================== */
#define TD5_TG_MESH_DISK_SIZE  0x38
#define TD5_TG_CMD_SIZE        16
#define TD5_TG_VTX_SIZE        44
/* Down-track sub-quads per span. A single 1500-unit quad shimmers at distance;
 * the Python emitter uses 3 for the same reason. */
#define TD5_TG_ROAD_SUBDIV     3
#define TD5_TG_SPANS_PER_ENTRY 4     /* entry = span >> 2 */

static void tg_put_f32(TG_Buf *buf, double v)
{
    float f = (float)v;
    unsigned int u;
    memcpy(&u, &f, sizeof(u));
    tg_put_u32(buf, u);
}

/* Left and right road edge at fraction f along span si. */
static void tg_road_edge(const TG_NodeList *nl, int si, double f, double shift,
                         double wscale,
                         double *lx, double *ly, double *lz,
                         double *rx, double *ry, double *rz)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *b = &nl->v[si + 1];
    double x = a->x + (b->x - a->x) * f;
    double y = a->y + (b->y - a->y) * f;
    double z = a->z + (b->z - a->z) * f;
    double w = (a->width + (b->width - a->width) * f) * wscale;
    double tx = a->tx + (b->tx - a->tx) * f;
    double tz = a->tz + (b->tz - a->tz) * f;
    double m = sqrt(tx * tx + tz * tz);
    if (m < 1e-9) { tx = 0.0; tz = 1.0; m = 1.0; }
    tx /= m; tz /= m;
    /* Left of travel is (tz, -tx), matching the strip row order. `shift` slides
     * the whole cross-section along that axis and `wscale` scales its width --
     * 1.0/0 for the plain main road, 0.5 + a half-width offset for the split
     * fork carriageways, so the road mesh follows the strip rows exactly. */
    x += tz * shift; z -= tx * shift;
    *lx = x + tz * (w * 0.5); *ly = y; *lz = z - tx * (w * 0.5);
    *rx = x - tz * (w * 0.5); *ry = y; *rz = z + tx * (w * 0.5);
}

/* One road mesh for span si, width scaled by wscale and laterally offset by
 * shift_near..shift_far across the span (1.0, 0, 0 = the plain full road).
 * Appended to blk. Returns 0 on OOM. */
static int tg_emit_road_quad(const TG_NodeList *nl, int si, int lanes,
                             double shift_near, double shift_far, double wscale,
                             TG_Buf *blk)
{
    double px[TD5_TG_ROAD_SUBDIV * 4], py[TD5_TG_ROAD_SUBDIV * 4];
    double pz[TD5_TG_ROAD_SUBDIV * 4], uu[TD5_TG_ROAD_SUBDIV * 4];
    double vv[TD5_TG_ROAD_SUBDIV * 4];
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int k, i, n = 0;

    for (k = 0; k < TD5_TG_ROAD_SUBDIV; k++) {
        double f0 = (double)k / (double)TD5_TG_ROAD_SUBDIV;
        double f1 = (double)(k + 1) / (double)TD5_TG_ROAD_SUBDIV;
        double nlx, nly, nlz, nrx, nry, nrz;
        double flx, fly, flz, frx, fry, frz;
        double s0v = shift_near + (shift_far - shift_near) * f0;
        double s1v = shift_near + (shift_far - shift_near) * f1;
        tg_road_edge(nl, si, f0, s0v, wscale, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
        tg_road_edge(nl, si, f1, s1v, wscale, &flx, &fly, &flz, &frx, &fry, &frz);
        /* Quad loop: near-left, near-right, far-right, far-left. */
        px[n]=nlx; py[n]=nly; pz[n]=nlz; uu[n]=0.0;           vv[n]=si+f0; n++;
        px[n]=nrx; py[n]=nry; pz[n]=nrz; uu[n]=(double)lanes; vv[n]=si+f0; n++;
        px[n]=frx; py[n]=fry; pz[n]=frz; uu[n]=(double)lanes; vv[n]=si+f1; n++;
        px[n]=flx; py[n]=fly; pz[n]=flz; uu[n]=0.0;           vv[n]=si+f1; n++;
    }

    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;   /* NaN/<=0 is rejected by the culler */

    /* --- mesh record (0x38) --- */
    tg_put_u16(blk, 259);            /* 0x00 render_type (nothing reads it) */
    tg_put_u16(blk, 0);              /* 0x02 billboard tag: 0 = opaque */
    tg_put_u32(blk, 1);              /* 0x04 command_count */
    tg_put_u32(blk, (unsigned)n);    /* 0x08 total_vertex_count */
    tg_put_f32(blk, radius);         /* 0x0C bounding_radius */
    tg_put_f32(blk, cx);             /* 0x10 bounding_center */
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0);            /* 0x1C origin: 0 for opaque geometry */
    tg_put_f32(blk, 0.0);
    tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);              /* 0x28 reserved */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);                       /* commands */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);     /* vertices */
    tg_put_u32(blk, 0);              /* 0x34 normals: NULL is allowed */

    /* --- one command: quads only, sequential vertex cursor --- */
    tg_put_u16(blk, 0);              /* dispatch_type 0 = TRISTRIP */
    tg_put_u16(blk, 0);              /* texture_page_id: the SAMPLED page */
    tg_put_u32(blk, 0);              /* reserved */
    tg_put_u16(blk, 0);                             /* triangle_count */
    tg_put_u16(blk, TD5_TG_ROAD_SUBDIV);            /* quad_count */
    tg_put_u32(blk, 0);              /* vertex_data_ptr 0 = sequential */

    /* --- de-indexed vertices, 44 B each --- */
    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0);        /* view xyz: filled at runtime */
        tg_put_f32(blk, 0.0);
        tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);/* lighting ARGB: full bright */
        tg_put_f32(blk, uu[i]);      /* UVs are normalised floats; >1 tiles */
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0);        /* proj_u/proj_v: runtime */
        tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* Plain main-road mesh for span si (no lateral offset). */
static int tg_emit_road_mesh(const TG_NodeList *nl, int si, int lanes,
                             TG_Buf *blk)
{
    return tg_emit_road_quad(nl, si, lanes, 0.0, 0.0, 1.0, blk);
}

/* Six quads. Corner sign triples per face, then which half-extents span the
 * face (for UV scaling). Winding is effectively free -- clip_and_submit_polygon
 * culls by screen area, and scenery is submitted CULL_NONE. */
static const signed char k_box_corner[6][4][3] = {
    {{-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1}},   /* +Y top    */
    {{-1,-1, 1},{ 1,-1, 1},{ 1,-1,-1},{-1,-1,-1}},   /* -Y bottom */
    {{-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1}},   /* -Z        */
    {{ 1,-1, 1},{-1,-1, 1},{-1, 1, 1},{ 1, 1, 1}},   /* +Z        */
    {{-1,-1, 1},{-1,-1,-1},{-1, 1,-1},{-1, 1, 1}},   /* -X        */
    {{ 1,-1,-1},{ 1,-1, 1},{ 1, 1, 1},{ 1, 1,-1}}    /* +X        */
};
/* Half-extent axis pairs giving each face's (u,v) size: 0=x 1=y 2=z. */
static const signed char k_box_uv_axis[6][2] = {
    {0,2},{0,2},{0,1},{0,1},{2,1},{2,1}
};

/* A box (building / bridge pier / tunnel wall) centred at c with half-extents
 * h, textured from `page`, tiling every `tile` world units. */
static int tg_emit_box_mesh(TG_Buf *blk, double cx, double cy, double cz,
                            double hx, double hy, double hz,
                            double fx, double fz, int page, double tile)
{
    /* Box frame: fwd = (fx,fz) along the road, right = (fz,-fx), up = +Y.
     * An axis-aligned box is fine for a building but wrong for anything that
     * must FOLLOW a curving road (tunnel walls, bridge decks), so hz runs
     * along the road and hx across it. */
    const double rx = fz, rz = -fx;
    const double hv[3] = { hx, hy, hz };
    double radius = sqrt(hx*hx + hy*hy + hz*hz);
    int f, i;

    if (!(radius > 0.0)) radius = 1.0;
    if (tile <= 0.0) tile = 1500.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, 6 * 4);                /* 6 quads, de-indexed */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    tg_put_u16(blk, 6);                    /* quad_count */
    tg_put_u32(blk, 0);

    for (f = 0; f < 6; f++) {
        double ua = 2.0 * hv[(int)k_box_uv_axis[f][0]] / tile;
        double vb = 2.0 * hv[(int)k_box_uv_axis[f][1]] / tile;
        for (i = 0; i < 4; i++) {
            const double sx = k_box_corner[f][i][0];
            const double sy = k_box_corner[f][i][1];
            const double sz = k_box_corner[f][i][2];
            tg_put_f32(blk, cx + rx * (sx * hx) + fx * (sz * hz));
            tg_put_f32(blk, cy +       sy * hy);
            tg_put_f32(blk, cz + rz * (sx * hx) + fz * (sz * hz));
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
            tg_put_u32(blk, 0xFFFFFFFFu);
            /* Corner order walks the quad loop, so (0,0)(u,0)(u,v)(0,v). */
            tg_put_f32(blk, (i == 1 || i == 2) ? ua : 0.0);
            tg_put_f32(blk, (i >= 2) ? vb : 0.0);
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        }
    }
    return !blk->oom;
}

/* A camera-facing billboard: the natural primitive for a tree, where a box
 * reads as a hedge slab. Two things differ from the opaque box path:
 *   - the mesh header field at 0x02 is a BILLBOARD TAG, not a page id: 1 or 2
 *     makes the renderer face the quad at the camera (td5_render_mesh.c:1720);
 *   - `origin` carries the WORLD position in 24.8, where opaque geometry
 *     leaves it zero (td5_render_mesh.c:1666), so the vertices below are LOCAL
 *     offsets about that origin -- x across, y up from the base.
 * The sampled page still comes from the command, as everywhere else.
 *
 * [UNCERTAIN] The transparent key index for an alpha-keyed page is not
 * documented in what I read; palette index 0 is reserved for it here. If the
 * engine keys on something else the surround will show as a solid block rather
 * than cutting out -- visible immediately, and diagnosable. */
static int tg_emit_billboard_mesh(TG_Buf *blk, double wx, double wy, double wz,
                                  double half_w, double height, int page)
{
    double radius = sqrt(half_w * half_w + height * height);
    int i;
    static const double lx[4] = { -1.0,  1.0,  1.0, -1.0 };
    static const double ly[4] = {  0.0,  0.0,  1.0,  1.0 };

    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 1);                    /* 1 = camera-facing billboard */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, 4);                    /* one quad */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, wx);                   /* bounding centre stays world */
    tg_put_f32(blk, wy + height * 0.5);
    tg_put_f32(blk, wz);
    tg_put_f32(blk, wx * 256.0);           /* origin is 24.8 for billboards */
    tg_put_f32(blk, wy * 256.0);
    tg_put_f32(blk, wz * 256.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, 1);                    /* quad_count */
    tg_put_u32(blk, 0);

    for (i = 0; i < 4; i++) {
        tg_put_f32(blk, lx[i] * half_w);   /* local: x across, y up */
        tg_put_f32(blk, ly[i] * height);
        tg_put_f32(blk, 0.0);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, (i == 1 || i == 2) ? 1.0 : 0.0);
        tg_put_f32(blk, (i >= 2) ? 0.0 : 1.0);   /* v flipped: base at v=1 */
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* ===================== FACADE WALLS =====================
 * Shipped TD5 buildings are NOT closed boxes with a UV-wrapped masonry texture.
 * A survey of level014 (Melbourne, 1743 meshes) found exactly ONE 6-quad box:
 * every building front is a FLAT array of road-facing quads, and every shipped
 * vertex UV is inside [0,1] -- pages are mapped ONCE per quad, never wrapped. A
 * wide/tall frontage is built by REPEATING GEOMETRY (many whole-page cells side
 * by side), not by stretching one page across a big face. That is why our boxes
 * read as "cut off in the middle": tg_emit_box_mesh sets UV = 2*half/tile > 1,
 * slicing the facade page mid-window at each edge.
 *
 * The facade path below imitates the shipped look: a grid of whole-page cells
 * laid flat along the road into a continuous street wall, so the texture is what
 * sizes the building (one page image per cell) and nothing is ever cut. */
#define TD5_TG_FACADE_MAXQUAD 160  /* both sides: front grid + two deep returns */

/* Write ONE opaque quad-list mesh in the 0x38 format, split into up to `nseg`
 * COMMANDS -- each command samples its own page over its own run of quads, in
 * vertex order. That is how one building can carry a shop page on the ground
 * floor and a wall page above without a second mesh (which would corrupt the
 * one-mesh-per-building offset accounting in tg_emit_models). */
static int tg_write_quad_mesh(TG_Buf *blk, const double *px, const double *py,
                              const double *pz, const double *uu, const double *vv,
                              int n, const int *seg_page, const int *seg_nq,
                              int nseg)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s;

    if (n <= 0 || nseg <= 0) return 1;
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
    tg_put_u32(blk, (unsigned)nseg);       /* command_count */
    tg_put_u32(blk, (unsigned)n);          /* total de-indexed vertices */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + nseg * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < nseg; s++) {
        tg_put_u16(blk, 0);                    /* dispatch_type 0 */
        tg_put_u16(blk, (unsigned)seg_page[s]);
        tg_put_u32(blk, 0);
        tg_put_u16(blk, 0);                    /* triangle_count */
        tg_put_u16(blk, (unsigned)seg_nq[s]);  /* quad_count */
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]);
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* Push floors [r0,r1) of a cols x `rows` grid onto the vertex arrays. `base` is
 * the lower-near corner; `across` and `up` are the FULL edge vectors (up spans
 * all `rows` floors). Each cell maps the whole page (UV 0..1, v=1 at the base),
 * so no image is cut mid-cell. Splitting the row range lets the ground floor go
 * on a shop page and the floors above on the wall page. */
static void tg_facade_push_grid(double bx, double by, double bz,
                                double ax, double ay, double az,
                                double ux, double uy, double uz,
                                int cols, int rows, int r0, int r1,
                                double *px, double *py, double *pz,
                                double *uu, double *vv, int *pn)
{
    int c, r, n = *pn;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    for (r = r0; r < r1; r++) {
        for (c = 0; c < cols; c++) {
            double c0 = (double)c / cols, c1 = (double)(c + 1) / cols;
            double r0f = (double)r / rows, r1f = (double)(r + 1) / rows;
            double rr0 = r0f, rr1 = r1f;
            if (n + 4 > TD5_TG_FACADE_MAXQUAD * 4) { *pn = n; return; }
            /* quad loop: near-bottom, far-bottom, far-top, near-top */
            px[n]=bx+ax*c0+ux*rr0; py[n]=by+ay*c0+uy*rr0; pz[n]=bz+az*c0+uz*rr0; uu[n]=0.0; vv[n]=1.0; n++;
            px[n]=bx+ax*c1+ux*rr0; py[n]=by+ay*c1+uy*rr0; pz[n]=bz+az*c1+uz*rr0; uu[n]=1.0; vv[n]=1.0; n++;
            px[n]=bx+ax*c1+ux*rr1; py[n]=by+ay*c1+uy*rr1; pz[n]=bz+az*c1+uz*rr1; uu[n]=1.0; vv[n]=0.0; n++;
            px[n]=bx+ax*c0+ux*rr1; py[n]=by+ay*c0+uy*rr1; pz[n]=bz+az*c0+uz*rr1; uu[n]=0.0; vv[n]=0.0; n++;
        }
    }
    *pn = n;
}

/* Is a facade wall present at span si on this side? A deterministic run/gap
 * pattern: spans group into periods; a per-period hash carves a leading GAP
 * (a side street) and the rest of the period is a continuous built RUN. The two
 * sides break at different spans (the +777 offset) so a street is never gapped
 * on both sides at once. */
#define TD5_TG_FACADE_PERIOD 13
static int tg_facade_built(int si, int left)
{
    unsigned int s     = (unsigned)si + (left ? 777u : 0u);
    unsigned int block = s / TD5_TG_FACADE_PERIOD;
    unsigned int phase = s % TD5_TG_FACADE_PERIOD;
    unsigned int gap   = 4u + ((block * 2654435761u) >> 29);   /* 4..11 */
    return (int)(phase >= gap);   /* runs 2..9 spans -> ~40% frontage built */
}

/* Which facade page a RUN uses -- keyed to the run hash so a whole building is
 * one texture but neighbouring buildings differ, the way a real street mixes
 * stone/glass/brick frontages. Variant 0 is the base WALL page; 1..N-1 are the
 * extra pages appended after GROUND. */
static int tg_facade_page(unsigned int gh)
{
    unsigned int v = (gh >> 17) % (unsigned)TD5_TG_WALL_VARIANTS;
    return v == 0 ? TD5_TG_PAGE_WALL : (TD5_TG_PAGE_WALL_EXTRA + (int)v - 1);
}

/* Which shop page a run's ground floor uses -- a different hash bit than the
 * wall page so the storefront and the tower above are chosen independently. */
static int tg_store_page(unsigned int gh)
{
    return TD5_TG_PAGE_STORE + (int)((gh >> 23) % (unsigned)TD5_TG_STORE_VARIANTS);
}

/* ===================== TREES =====================
 * A palette of tree/palm/conifer/topiary SPECIES. Each entry is a billboard
 * size (raw = world_units*256) plus a procedural silhouette shape used when
 * real textures are off. Real mode fills each variant page from a shipped TD5
 * foliage page (level ids in the comments); index 0 of those pages is the
 * transparent key. Biomes reference these by index (tree_set) and mix them. */
enum { TG_TREE_DECID = 0, TG_TREE_CONIFER, TG_TREE_PALM, TG_TREE_TOPIARY,
       TG_TREE_WILLOW };
typedef struct { int w, h, shape; } TG_TreePage;
static const TG_TreePage k_tree_pages[TD5_TG_TREE_VARIANTS] = {
    { 4200, 5600, TG_TREE_DECID   },  /* 0  L017 p266  deciduous            */
    { 5200, 6600, TG_TREE_DECID   },  /* 1  L008 p173  big deciduous        */
    { 4000, 5400, TG_TREE_DECID   },  /* 2  L013 p234  deciduous            */
    { 3200, 6400, TG_TREE_CONIFER },  /* 3  L003 p441  conifer              */
    { 3200, 6400, TG_TREE_CONIFER },  /* 4  L003 p445  snow conifer         */
    { 3400, 7200, TG_TREE_PALM    },  /* 5  L026 p097  palm                 */
    { 3800, 6800, TG_TREE_PALM    },  /* 6  L014 p249  palm                 */
    { 3600, 4200, TG_TREE_TOPIARY },  /* 7  L004 p359  topiary              */
    { 3600, 5000, TG_TREE_TOPIARY },  /* 8  L004 p360  topiary              */
    { 4200, 6000, TG_TREE_WILLOW  }   /* 9  L004 p369  weeping willow       */
};

/* TEXTURES.DAT page slot for tree variant v (variant 0 reuses PAGE_TREE). */
static int tg_tree_slot(int v)
{
    return v == 0 ? TD5_TG_PAGE_TREE : (TD5_TG_PAGE_TREE_EXTRA + v - 1);
}

/* ===================== BIOMES =====================
 * A biome owns a RUN of spans and drives what stands beside the road: how
 * dense the props are, how tall, how far back, and which texture page. That is
 * what makes a stretch of city read differently from open fields without
 * needing separate emitters per biome -- every prop is still a box.
 *
 * Not yet driven by biome: the section mix (straight/curve/acute weighting).
 * The section picker runs during the centerline walk, before any of this, so
 * per-biome cornering needs the picker to know its own position first. */
/* Facade/tree sizes below are the SHIPPED-track measurements (level014 city,
 * level005 rural), in raw 24.8 units == world_units * 256. A survey found a
 * facade page-cell is ~8.4 x 11.5 wu (2150 x 2950 raw), a city frontage ~3
 * floors tall, buildings sit ON the curb (setback ~0), and city trees are big
 * and sparse while rural trees are small and dense. */
#define TD5_TG_TREESET_MAX 4
typedef struct {
    const char *name;
    int    density;      /* trees: prop if (hash>>28) <= this, so 0..15 */
    int    cell_w, cell_h;      /* facade page-cell world size, raw */
    int    floors_min, floors_extra; /* facade height in cells */
    int    depth;        /* building depth for side returns, raw */
    int    sidewalk;     /* setback from road edge, raw (~0 = on the curb) */
    int    tree_set[TD5_TG_TREESET_MAX]; /* tree-variant indices to mix */
    int    tree_n;       /* how many entries of tree_set are used (0 = facade) */
    int    page;         /* facade WALL page for box biomes */
    int    tower_mask;   /* (hash>>3 & mask)==0 -> a taller run (tower cluster) */
    int    billboard;    /* 1 = camera-facing trees, 0 = facade wall */
    int    ground_page;  /* page for the terrain slab under/around the road */
} TG_Biome;

static const TG_Biome k_biomes[] = {
    /* CITY: ~8.4x11.5 wu cells, ~3 floors, on the curb, big sparse towers. */
    { "CITY",       9, 2150, 2950, 2, 3, 6000, 350, {0}, 0,
      TD5_TG_PAGE_WALL,   3, 0, TD5_TG_PAGE_GROUND },
    /* FIELDS: sparse deciduous on an open horizon. */
    { "FIELDS",     2, 0,0,0,0,0,0, {2, 0},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN },
    /* FOREST: dense mixed deciduous crowding the verge. */
    { "FOREST",    11, 0,0,0,0,0,0, {0, 1, 2},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN },
    /* INDUSTRIAL: wider squat sheds (~10 wu cells), 1-2 floors, deeper. */
    { "INDUSTRIAL", 6, 2560, 2300, 1, 2, 8000, 600, {0}, 0,
      TD5_TG_PAGE_WALL,  63, 0, TD5_TG_PAGE_GROUND },
    /* ALPINE: conifers + snow conifers. */
    { "ALPINE",     8, 0,0,0,0,0,0, {3, 4},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN },
    /* COAST: palms. */
    { "COAST",      5, 0,0,0,0,0,0, {5, 6},       2,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN },
    /* ORIENTAL: manicured topiary + weeping willow. */
    { "ORIENTAL",   9, 0,0,0,0,0,0, {7, 8, 9},    3,
      TD5_TG_PAGE_TREE,  255, 1, TD5_TG_PAGE_GREEN }
};
#define TD5_TG_BIOME_COUNT 7
#define TD5_TG_BIOME_RUN   150

static int tg_biome_for_span(int si)
{
    unsigned int h = (unsigned)(si / TD5_TG_BIOME_RUN) * 2654435761u;
    return (int)((h >> 27) % TD5_TG_BIOME_COUNT);
}

/* A continuous street wall of flat facade cells lining span si -- one mesh,
 * both sides. A side is built per tg_facade_built (runs separated by side
 * streets). Consecutive built spans share their near/far endpoints, so the
 * facades ABUT into an unbroken wall the way a shipped city block does.
 *
 * Single- vs multi-sided is POSITIONAL, exactly as in the shipped data: a
 * run-INTERIOR span shows only its road-facing plane (single-sided); a run-END
 * span (its neighbour on that side is a gap) also gets a RETURN cap turning the
 * corner, so the wall does not read as a paper edge (multi-sided). Setback and
 * base height are keyed to the RUN, not the span, so a wall stays straight and a
 * block shares a rough height while individual buildings still step. */
typedef struct {
    int    built;
    double bx, by, bz, ax, ay, az, H, depth;
    double lx0, lz0, lx1, lz1;
    int    cols, rows, cap_near, cap_far;
} TG_SideGeom;

/* Geometry for one side (0=right,1=left) of the wall at span si. built=0 when
 * the run/gap pattern or the branch-corridor exclusion skips this side. */
static void tg_side_geom(const TG_NodeList *nl, int si, int left,
                         const TG_Biome *b, TG_SideGeom *g)
{
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1 = &nl->v[si + 1];
    const double side = left ? 1.0 : -1.0;
    unsigned int gh;
    double set0, set1, flen;
    int floors;

    g->built = 0;
    if (!tg_facade_built(si, left)) return;
    if (tg_branches_enabled() && side < 0.0) {
        const int lo = TD5_TG_BRANCH_FORK_SPAN - TD5_TG_BRANCH_WIDEN;
        const int hi = TD5_TG_BRANCH_FORK_SPAN + TD5_TG_BRANCH_LEN + 1;
        if (si >= lo && si <= hi) return;
    }

    /* Height is a whole number of FLOORS, keyed to the RUN so a building is one
     * uniform block that steps at the next side street. */
    gh     = (((unsigned)si + (left ? 777u : 0u)) / TD5_TG_FACADE_PERIOD)
             * 2246822519u;
    floors = b->floors_min + (int)((gh >> 7) % (unsigned)b->floors_extra);
    if (b->tower_mask && ((gh >> 3) & (unsigned)b->tower_mask) == 0)
        floors += 1 + (int)((gh >> 11) % 4);          /* whole-run tower cluster */
    g->rows  = floors;
    g->H     = (double)floors * (double)b->cell_h;
    g->depth = (double)b->depth;

    g->lx0 = n0->tz * side; g->lz0 = -n0->tx * side;
    g->lx1 = n1->tz * side; g->lz1 = -n1->tx * side;
    set0 = n0->width * 0.5 + (double)b->sidewalk;
    set1 = n1->width * 0.5 + (double)b->sidewalk;

    g->bx = n0->x + g->lx0 * set0; g->by = n0->y; g->bz = n0->z + g->lz0 * set0;
    g->ax = (n1->x + g->lx1 * set1) - g->bx;
    g->ay = n1->y - n0->y;
    g->az = (n1->z + g->lz1 * set1) - g->bz;

    flen = sqrt(g->ax * g->ax + g->az * g->az);
    if (flen < 1.0) flen = 1.0;
    g->cols = (int)(flen / (double)b->cell_w + 0.5);
    if (g->cols < 1) g->cols = 1;
    if (g->cols > 4) g->cols = 4;

    g->cap_near = !tg_facade_built(si - 1, left);
    g->cap_far  = !tg_facade_built(si + 1, left);
    g->built = 1;
}

static int tg_emit_street_wall(const TG_NodeList *nl, int si,
                               const TG_Biome *b, TG_Buf *blk)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    TG_SideGeom sd[2];
    unsigned int block_gh = (unsigned)(si / TD5_TG_FACADE_PERIOD) * 2246822519u;
    int n = 0, n_store, s, nseg, seg_page[2], seg_nq[2];

    if (si + 1 >= nl->count) return 1;    /* need the far endpoint to abut */
    tg_side_geom(nl, si, 0, b, &sd[0]);
    tg_side_geom(nl, si, 1, b, &sd[1]);

    /* STOREFRONT pass: the ground floor (row 0) of each front plane goes FIRST
     * in the vertex list, so a leading command can bind the shop page. */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 0, 1,
                            px, py, pz, uu, vv, &n);
    }
    n_store = n;

    /* FACADE pass: the floors ABOVE the shop, plus the deep return walls that
     * turn the corner at run ends (the multi-sided buildings). */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 1, g->rows,
                            px, py, pz, uu, vv, &n);
        if (g->cap_near)
            tg_facade_push_grid(g->bx, g->by, g->bz,
                                g->lx0 * g->depth, 0.0, g->lz0 * g->depth,
                                0.0, g->H, 0.0, 2, g->rows, 0, g->rows,
                                px, py, pz, uu, vv, &n);
        if (g->cap_far)
            tg_facade_push_grid(g->bx + g->ax, g->by + g->ay, g->bz + g->az,
                                g->lx1 * g->depth, 0.0, g->lz1 * g->depth,
                                0.0, g->H, 0.0, 2, g->rows, 0, g->rows,
                                px, py, pz, uu, vv, &n);
    }

    if (n <= 0) return 1;
    /* Pages keyed to the PERIOD-block (a run lives inside one block) so a whole
     * building keeps one shop + one wall page and neighbours differ. Ground
     * quads [0,n_store) sample the shop page, the rest the wall page. */
    if (n_store > 0 && n_store < n) {
        seg_page[0] = tg_store_page(block_gh);  seg_nq[0] = n_store / 4;
        seg_page[1] = tg_facade_page(block_gh); seg_nq[1] = (n - n_store) / 4;
        nseg = 2;
    } else {
        seg_page[0] = (n_store >= n) ? tg_store_page(block_gh)
                                     : tg_facade_page(block_gh);
        seg_nq[0] = n / 4;
        nseg = 1;
    }
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg);
}

/* Scenery beside span si, or 0 (no-op success) if this span gets none. One mesh
 * at most, deterministic from si -- NOT the shared RNG, which the centerline
 * walk has already consumed, so scenery cannot perturb track shape. Tree biomes
 * keep the camera-facing billboard; box biomes now lay a flat facade wall
 * (tg_emit_street_wall) instead of a UV-tiled 6-sided box. */
static int tg_building_for_span(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    unsigned int h = (unsigned)si * 2654435761u;
    const TG_Node *n;
    const TG_Biome *b;
    const TG_TreePage *tp;
    double side, gap, tw, th, jit, lx, lz, cx, cz;
    int tv;

    if (si <= TD5_TG_GRID_SPAN) return 1;      /* keep the grid area clear */
    b = &k_biomes[tg_biome_for_span(si)];

    if (!b->billboard || b->tree_n <= 0)
        return tg_emit_street_wall(nl, si, b, blk);

    /* Trees: density-gated camera-facing billboards. Each biome MIXES several
     * species (tree_set) picked per-tree, each with its own shipped size, and a
     * scale jitter that keeps the page aspect. */
    if ((int)(h >> 28) > b->density) return 1;

    n = &nl->v[si];
    side = ((h >> 3) & 1) ? 1.0 : -1.0;
    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
    tp  = &k_tree_pages[tv];

    jit = 0.8 + (double)((h >> 9) % 41) * 0.01;    /* 0.80 .. 1.20 */
    tw  = (double)tp->w * jit;
    th  = (double)tp->h * jit;
    gap = 800.0 + (double)((h >> 5) % 2400);       /* set back off the verge */

    lx = n->tz * side; lz = -n->tx * side;
    cx = n->x + lx * (n->width * 0.5 + gap + tw * 0.5);
    cz = n->z + lz * (n->width * 0.5 + gap + tw * 0.5);

    return tg_emit_billboard_mesh(blk, cx, n->y, cz, tw * 0.5, th,
                                  tg_tree_slot(tv));
}

/* Tunnels come in runs so a whole stretch is enclosed, not isolated spans. */
#define TD5_TG_TUNNEL_RUN  20

static int tg_span_in_tunnel(int si)
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
    if (!td5_env_flag_off("TD5RE_AUTOTRACK_TUNNELS")) return 0;
    if (si <= TD5_TG_GRID_SPAN + 40) return 0;      /* not right off the grid */
    h = (unsigned)(si / TD5_TG_TUNNEL_RUN) * 2246822519u;
    return ((h >> 29) == 0);                        /* ~1 run in 8 */
}

/* Tunnel cross-section at span si: two side walls plus a roof, each its own
 * mesh so per-mesh frustum culling cannot pop the whole tunnel at once.
 * Oriented to the road tangent, one span long with a slight overlap. */
static int tg_emit_tunnel(const TG_NodeList *nl, int si, TG_Buf *blk,
                          int *added)
{
    const TG_Node *n = &nl->v[si];
    const double wall_t = 300.0;
    const double height = 2600.0;
    const double side_x = n->width * 0.5 + wall_t;
    const double lx = n->tz, lz = -n->tx;
    int i;

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
        if (!tg_emit_box_mesh(blk, n->x + ox, cy, n->z + oz,
                              hx, hy, 780.0, n->tx, n->tz,
                              TD5_TG_PAGE_WALL, 3000.0))
            return 0;
        (*added)++;
    }
    return 1;
}

/* How far above the LOCAL terrain line counts as ELEVATED. Single definition:
 * both the bridge deck and the guardrail gate key off it, so "on a bridge"
 * cannot mean two different things in two places.
 *
 * 900 is a GEOMETRY fact, not a tuning dial: the deck is 780 tall and the pier
 * is lift*0.5, so below roughly this much lift the deck sits in the road and
 * the pier is too short to see. Do not lower it to make bridges appear.
 *
 * MEASURED CONSEQUENCE (3 seeds, 1800 spans each): against the local terrain
 * line, this generator's elevation almost never reaches 900 of local
 * convexity -- seed 123456789 peaks near 160, seed 42 near 200, and seed 777
 * does not reach even 100. So bridges now essentially never emit, and that is
 * CORRECT: the previous 87%-of-all-spans bridging was an artifact of measuring
 * against the global track minimum, which made every hill a bridge.
 *
 * To make bridges a real feature again they must be PLACED DELIBERATELY, the
 * way tunnels and branch forks are (a chosen span range, with the elevation
 * driven to suit), not conjured by lowering this threshold. */
#define TD5_TG_BRIDGE_MIN_LIFT 900.0

/* Half-width, in spans, of the window the local terrain line is taken over.
 * ~8 spans either side = the length of road a deck would plausibly carry. */
#define TD5_TG_LIFT_WINDOW 8

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
static double tg_local_ground_y(const TG_NodeList *nl, int si)
{
    const int w = td5_env_int("TD5RE_AUTOTRACK_LIFT_WINDOW",
                              TD5_TG_LIFT_WINDOW, 1, 200);
    int a = si - w;
    int b = si + w;
    if (a < 0) a = 0;
    if (b > nl->count - 1) b = nl->count - 1;
    return (nl->v[a].y + nl->v[b].y) * 0.5;
}

/* Bridge: where the road humps above the local terrain line, put a deck slab
 * just under it and a pier down to that line. Purely cosmetic -- the driving
 * surface is still the STRIP. */
static int tg_emit_bridge(const TG_NodeList *nl, int si,
                          TG_Buf *blk, int *added)
{
    const TG_Node *n = &nl->v[si];
    const double ref  = tg_local_ground_y(nl, si);
    const double lift = n->y - ref;
    const int deliberate = tg_span_in_bridge_run(si);

    /* A PLACED run always gets its deck -- the range decides, so a later grade
     * or amplitude change cannot silently delete every bridge the way keying
     * purely off lift did. The organic lift test is kept as a second route so
     * a genuinely convex crest still gets a deck if one ever occurs. */
    if (!deliberate && lift < TD5_TG_BRIDGE_MIN_LIFT) return 1;

    if (!tg_emit_box_mesh(blk, n->x, n->y - 300.0, n->z,
                          n->width * 0.5 + 250.0, 200.0, 780.0,
                          n->tx, n->tz, TD5_TG_PAGE_WALL, 3000.0))
        return 0;
    (*added)++;

    if ((si & 3) == 0) {                            /* a pier every 4th span */
        /* Floor the height: at the very ends of a placed run the hump is still
         * near zero, and a pier a few units tall is invisible clutter that
         * still costs a mesh. */
        double pier_h = lift * 0.5;
        if (pier_h < 150.0) pier_h = 150.0;
        if (!tg_emit_box_mesh(blk, n->x, ref + pier_h, n->z,
                              700.0, pier_h, 700.0,
                              n->tx, n->tz, TD5_TG_PAGE_WALL, 3000.0))
            return 0;
        (*added)++;
    }
    return 1;
}

/* GROUND. Without this the road is a ribbon in a void: buildings and trees
 * stand on nothing and every frame reads as objects floating in blue. One wide
 * flat slab per display-list ENTRY (not per span -- per-span slabs would
 * overlap into heavy overdraw for no gain), sitting just below the road
 * surface and extending well past both verges.
 *
 * Textured from the biome's ground page, so FIELDS/FOREST get vegetation and
 * CITY/INDUSTRIAL get concrete. Cosmetic only: driving off the road still puts
 * you on nothing, because collision comes from the STRIP, not from this. */
#define TD5_TG_GROUND_WIDTH   24000.0   /* how far the verge extends outward */
#define TD5_TG_GROUND_DROP       70.0    /* just under the road, avoids z-fight */

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
static int tg_emit_ground(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
    const double drop = TD5_TG_GROUND_DROP;
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len;
    double cx, cy, cz, radius = 0.0;
    double px[8], py[8], pz[8], uu[8], vv[8];
    int i, n = 0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

    /* Outward direction = along the cross-section, away from the centre. */
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    /* ISOTROPIC UV. V advances one tile per span (~SPAN_LENGTH world units), so
     * the outer U is GROUND_WIDTH/SPAN_LENGTH to make each tile SQUARE. The old
     * U ran 0..4 across the full 24000-unit skirt -- a ~4:1 lateral stretch that
     * both smeared the texture and defeated the (isotropic, box-filter) mipmaps,
     * so the far ground shimmered/rippled. Square tiles let the mips minify
     * cleanly and the ground reads flat to the horizon. */
    const double u_out = TD5_TG_GROUND_WIDTH / (double)TD5_TG_SPAN_LENGTH;

    /* LEFT skirt: road edge -> outward. Loop order near-in, near-out, far-out,
     * far-in so the quad is a proper ring. The INNER edge sits at ROAD LEVEL
     * (nly, not nly-drop) so the terrain meets the asphalt flush -- the old
     * uniform -drop left the whole skirt 70 units below the road edge, a thin
     * void/lip between road and grass. Only the OUTER edge drops, giving a gentle
     * embankment away from the road. Inner and road edge share the same line
     * (they meet, they do not overlap), so there is no z-fight to avoid. */
    px[n]=nlx;                        py[n]=nly;      pz[n]=nlz;
    uu[n]=0.0;   vv[n]=(double)si;      n++;
    px[n]=nlx+nux*TD5_TG_GROUND_WIDTH; py[n]=nly-drop; pz[n]=nlz+nuz*TD5_TG_GROUND_WIDTH;
    uu[n]=u_out; vv[n]=(double)si;      n++;
    px[n]=flx+fux*TD5_TG_GROUND_WIDTH; py[n]=fly-drop; pz[n]=flz+fuz*TD5_TG_GROUND_WIDTH;
    uu[n]=u_out; vv[n]=(double)si+1.0;  n++;
    px[n]=flx;                        py[n]=fly;      pz[n]=flz;
    uu[n]=0.0;   vv[n]=(double)si+1.0;  n++;

    /* RIGHT skirt: mirrored, outward is -unit. */
    px[n]=nrx;                        py[n]=nry;      pz[n]=nrz;
    uu[n]=0.0;   vv[n]=(double)si;      n++;
    px[n]=nrx-nux*TD5_TG_GROUND_WIDTH; py[n]=nry-drop; pz[n]=nrz-nuz*TD5_TG_GROUND_WIDTH;
    uu[n]=u_out; vv[n]=(double)si;      n++;
    px[n]=frx-fux*TD5_TG_GROUND_WIDTH; py[n]=fry-drop; pz[n]=frz-fuz*TD5_TG_GROUND_WIDTH;
    uu[n]=u_out; vv[n]=(double)si+1.0;  n++;
    px[n]=frx;                        py[n]=fry;      pz[n]=frz;
    uu[n]=0.0;   vv[n]=(double)si+1.0;  n++;

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
    tg_put_u16(blk, (unsigned)b->ground_page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    tg_put_u16(blk, 2);                    /* two quads: left + right skirt */
    tg_put_u32(blk, 0);

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* GORE / MEDIAN fill for a split-fork span. The main (left) and branch (right)
 * half carriageways only touch at the fork and rejoin; where the branch bows
 * away, the strip between the main's right edge (road centre, lateral 0) and the
 * branch's left edge (branch_shift + width/4) has no road mesh and shows through
 * to the void. Fill it with a ground quad, just below road level so it reads as
 * a sunken median and does not z-fight the carriageway edges. Zero-width (hence
 * invisible) at the fork/rejoin where the two edges meet. `si` is the MAIN span;
 * shift_n/shift_f are the BRANCH lateral offsets at this span's ends. */
static int tg_emit_gore(const TG_NodeList *nl, int si,
                        double shift_n, double shift_f, TG_Buf *blk)
{
    const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];
    const TG_Node *a = &nl->v[si], *c = &nl->v[si + 1];
    const double drop = 20.0;
    double tnr = shift_n + a->width * 0.25;   /* branch left edge, near */
    double tfr = shift_f + c->width * 0.25;   /* branch left edge, far  */
    double px[4], py[4], pz[4], uu[4], vv[4];
    double cx = 0, cy = 0, cz = 0, radius = 0;
    int i;

    /* near-left = road centre, near-right = branch left edge, then far. */
    px[0]=a->x;             py[0]=a->y-drop; pz[0]=a->z;             uu[0]=0.0; vv[0]=(double)si;
    px[1]=a->x+a->tz*tnr;   py[1]=a->y-drop; pz[1]=a->z-a->tx*tnr;   uu[1]=1.0; vv[1]=(double)si;
    px[2]=c->x+c->tz*tfr;   py[2]=c->y-drop; pz[2]=c->z-c->tx*tfr;   uu[2]=1.0; vv[2]=(double)si+1.0;
    px[3]=c->x;             py[3]=c->y-drop; pz[3]=c->z;             uu[3]=0.0; vv[3]=(double)si+1.0;

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
    tg_put_u16(blk, (unsigned)b->ground_page);
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

/* ===================== GUARDRAILS =====================
 * The car is already contained by collision WALLS derived from the STRIP rail
 * vertices, but nothing draws them, so the road ends at an invisible boundary.
 * Guardrails do not add a constraint -- they make the existing one legible.
 *
 * PLACEMENT is the whole correctness question, and it is NOT assumed here.
 * td5_track_resolve_wall_contacts builds the left rail from row point 0 and the
 * right rail from row point `lane_count + k_rail_lut_[lr][type]`. For span_type
 * 1 -- the only type this generator emits -- BOTH LUT entries are 0, so the
 * rails are row points 0 and lane_count: the outermost points of the row, which
 * tg_emit_span_range places at -/+ width/2. tg_road_edge returns exactly those
 * two points, so deriving the barrier from it puts the visual where collision
 * actually stops you.
 *
 * Deriving from tg_road_edge rather than hardcoding width/2 also means the
 * barrier tracks width changes for free, and keeps ONE definition of "the road
 * edge" shared with the road mesh and the ground skirts. NOTE for the acute /
 * dual-carriageway work: emitting any span type other than 1 brings the LUT
 * offsets into play (types 2..7 are nonzero) and the right rail stops being the
 * outermost point -- this function would then need the same LUT.
 *
 * A BOX per span was rejected: a box cannot follow a curving or undulating
 * road, which is exactly why the ground moved from slabs to edge-derived
 * strips. This emits a proper 3-quad prism per side (inner face, outer face,
 * top cap) built from the span's own edge points, so it curves and climbs with
 * the road. A prism rather than a flat ribbon because a single quad may be
 * backface-culled from one side, and two opposite-wound coplanar quads would
 * z-fight if culling is off -- the prism is correct either way.
 */
#define TD5_TG_RAIL_HEIGHT     700.0   /* top of the barrier above road level */
#define TD5_TG_RAIL_BASE_DROP   60.0   /* start below the surface: no gap on
                                        * undulating spans */
#define TD5_TG_RAIL_THICK       55.0   /* prism depth, outward */
#define TD5_TG_RAIL_OFFSET      40.0   /* outboard of the rail line, so the
                                        * barrier does not share an edge with
                                        * the road surface and shimmer */

static int tg_guardrails_enabled(void)
{
    /* Default OFF until a frame confirms it, same discipline as tunnels and
     * branches. NOTE td5_env_flag_off() returns 1 only when the value is
     * literally "1" -- it means "opt in", despite the name. */
    return td5_env_flag_off("TD5RE_AUTOTRACK_GUARDRAILS");
}

/* Should span si carry a barrier? Not every span: real roads are lined on the
 * outside of bends and where the road is elevated, not down every straight, and
 * railing all 1800 spans would both look wrong and cost 1800 extra meshes. */
static int tg_span_needs_guardrail(const TG_NodeList *nl, int si, int nspans)
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

    /* EXCLUSION: the branch fork mouth and its widened approach. A barrier
     * across the corridor entrance would visually wall off the branch. */
    if (tg_branches_enabled()) {
        const int lo = TD5_TG_BRANCH_FORK_SPAN - TD5_TG_BRANCH_WIDEN - 2;
        const int hi = TD5_TG_BRANCH_FORK_SPAN + TD5_TG_BRANCH_LEN + 2;
        if (si >= lo && si <= hi) return 0;
    }
    (void)nspans;

    /* On a deliberately-placed deck: rail the WHOLE run, including the shallow
     * ends the lift test would miss. This is the case the guardrail work
     * existed for -- nothing stops you leaving a deck sideways. */
    if (tg_span_in_bridge_run(si)) return 1;

    /* Elevated: nothing stops you leaving a bridge deck, so rail it. Judged
     * against the LOCAL terrain line, so this means "on a deck" and not
     * merely "uphill of the lowest point on the track". */
    if (nl->v[si].y - tg_local_ground_y(nl, si) >= TD5_TG_BRIDGE_MIN_LIFT)
        return 1;

    /* Bend: heading change across this span, in tenths of a degree. */
    cross = nl->v[si].tx * nl->v[si + 1].tz - nl->v[si].tz * nl->v[si + 1].tx;
    dot   = nl->v[si].tx * nl->v[si + 1].tx + nl->v[si].tz * nl->v[si + 1].tz;
    ang_deg = atan2(fabs(cross), dot) * 180.0 / TD5_TG_PI;
    return (ang_deg * 10.0) >= (double)limit;
}

/* One barrier prism per side for span si. Caller gates with
 * tg_span_needs_guardrail. */
static int tg_emit_guardrail(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    double nlx, nly, nlz, nrx, nry, nrz;   /* near left / right road edge */
    double flx, fly, flz, frx, fry, frz;   /* far  left / right road edge */
    double nux, nuz, fux, fuz;             /* outward lateral units */
    double len, cx, cy, cz, radius = 0.0;
    double px[24], py[24], pz[24], uu[24], vv[24];
    int side, i, n = 0;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);

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
        const double o0 = TD5_TG_RAIL_OFFSET;
        const double o1 = TD5_TG_RAIL_OFFSET + TD5_TG_RAIL_THICK;
        /* near/far x inner/outer, at base and top. */
        const double nib_x = ex + s * nux * o0, nib_z = ez + s * nuz * o0;
        const double nob_x = ex + s * nux * o1, nob_z = ez + s * nuz * o1;
        const double fib_x = gx + s * fux * o0, fib_z = gz + s * fuz * o0;
        const double fob_x = gx + s * fux * o1, fob_z = gz + s * fuz * o1;
        const double nyb = ey - TD5_TG_RAIL_BASE_DROP;
        const double fyb = gy - TD5_TG_RAIL_BASE_DROP;
        const double nyt = ey + TD5_TG_RAIL_HEIGHT;
        const double fyt = gy + TD5_TG_RAIL_HEIGHT;
        const double u0 = (double)si, u1 = (double)si + 1.0;

        /* INNER face (towards the road). U runs along the road, V up the face,
         * so the page's bottom rows land at the base -- see the page comment. */
        px[n]=nib_x; py[n]=nyb; pz[n]=nib_z; uu[n]=u0; vv[n]=0.0; n++;
        px[n]=nib_x; py[n]=nyt; pz[n]=nib_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=fib_x; py[n]=fyt; pz[n]=fib_z; uu[n]=u1; vv[n]=1.0; n++;
        px[n]=fib_x; py[n]=fyb; pz[n]=fib_z; uu[n]=u1; vv[n]=0.0; n++;

        /* OUTER face, wound the other way so it faces away from the road. */
        px[n]=fob_x; py[n]=fyb; pz[n]=fob_z; uu[n]=u1; vv[n]=0.0; n++;
        px[n]=fob_x; py[n]=fyt; pz[n]=fob_z; uu[n]=u1; vv[n]=1.0; n++;
        px[n]=nob_x; py[n]=nyt; pz[n]=nob_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=nob_x; py[n]=nyb; pz[n]=nob_z; uu[n]=u0; vv[n]=0.0; n++;

        /* TOP cap, so the barrier reads as solid from a chase camera. */
        px[n]=nib_x; py[n]=nyt; pz[n]=nib_z; uu[n]=u0; vv[n]=1.0; n++;
        px[n]=nob_x; py[n]=nyt; pz[n]=nob_z; uu[n]=u0; vv[n]=0.85; n++;
        px[n]=fob_x; py[n]=fyt; pz[n]=fob_z; uu[n]=u1; vv[n]=0.85; n++;
        px[n]=fib_x; py[n]=fyt; pz[n]=fib_z; uu[n]=u1; vv[n]=1.0; n++;
    }

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
    tg_put_u16(blk, TD5_TG_PAGE_RAIL);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    tg_put_u16(blk, 6);                    /* 3 quads x 2 sides */
    tg_put_u32(blk, 0);

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]); tg_put_f32(blk, py[i]); tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]); tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

static int tg_emit_models(const TG_NodeList *nl, int nspans, int lanes,
                          TG_Buf *out)
{
    /* `nspans` is the FULL strip span count. With a branch it INCLUDES the pad +
     * corridor tail (layout: main 0..ring-1, pad at ring, corridor ring+1..
     * ring+LEN), so nspans = ring + 1 + LEN. The centerline `nl` only has the
     * main-ring nodes, so the corridor spans must take their geometry from the
     * base main node nl->v[F+1+k] plus the branch shift -- NOT from nl->v[si],
     * which is out of range there (the old code read OOB garbage for those spans,
     * emitting nothing visible, which is why the branch had no road). */
    const int branch_active = tg_branches_enabled() &&
        nspans > TD5_TG_BRANCH_FORK_SPAN + TD5_TG_BRANCH_LEN + 8;
    const int ring = branch_active ? nspans - 1 - TD5_TG_BRANCH_LEN : nspans;
    /* Native-faithful fork: the road SPLITS into two half-width carriageways --
     * MAIN (left, main_half lanes, +width/4) over ring spans F+1..F+LEN, and the
     * BRANCH (right, br_lanes, bowed) over the appended corridor. The fork span
     * F and rejoin R stay full width. The road meshes must match those strip
     * carriageways, not the full road. */
    const int F_fork    = TD5_TG_BRANCH_FORK_SPAN;
    const int main_half = lanes / 2;
    const int br_lanes  = lanes - main_half;
    const int nentries = (nspans + TD5_TG_SPANS_PER_ENTRY - 1)
                       / TD5_TG_SPANS_PER_ENTRY;
    /* Road meshes plus at most one building per span in the entry. */
    /* Per span: ground skirt + road + guardrail + building + up to 3 tunnel
     * pieces + up to 2 bridge pieces. */
    enum { TG_MAX_MESHES_PER_ENTRY = TD5_TG_SPANS_PER_ENTRY * 10 };
    const int rails = tg_guardrails_enabled();
    int nrails = 0;
    TG_Buf *blocks;
    unsigned int cursor;
    int e, ok = 1;

    blocks = (TG_Buf *)calloc((size_t)nentries, sizeof(TG_Buf));
    if (!blocks) return 0;

    for (e = 0; e < nentries && ok; e++) {
        const int s0 = e * TD5_TG_SPANS_PER_ENTRY;
        int ns = nspans - s0;
        size_t moff[TG_MAX_MESHES_PER_ENTRY];
        TG_Buf meshes;
        int nmesh = 0, i;

        if (ns > TD5_TG_SPANS_PER_ENTRY) ns = TD5_TG_SPANS_PER_ENTRY;
        memset(&meshes, 0, sizeof(meshes));

        /* Ground skirt then road, per span. Offsets are RECORDED as meshes are
         * appended -- sizes differ once ground, buildings and road quads are
         * mixed, so they cannot come from a uniform stride. */
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;

            /* Corridor span (ring < si <= ring+LEN): the BRANCH (right) half
             * carriageway only, at the bowed geometry. The pad span (si == ring)
             * carries nothing. The main-road ground skirt already extends under
             * the corridor, so no separate ground here. */
            if (si >= ring) {
                const int k = si - ring - 1;
                if (branch_active && k >= 0 && k < TD5_TG_BRANCH_LEN) {
                    moff[nmesh++] = meshes.len;
                    if (!tg_emit_road_quad(nl, F_fork + 1 + k, br_lanes,
                                           tg_branch_shift(k, nl->v[F_fork + 1 + k].width),
                                           tg_branch_shift(k + 1, nl->v[F_fork + 2 + k].width),
                                           0.5, &meshes))
                        ok = 0;
                }
                continue;
            }

            moff[nmesh++] = meshes.len;
            if (!tg_emit_ground(nl, si, &meshes)) { ok = 0; break; }
            moff[nmesh++] = meshes.len;
            /* MAIN carriageway is narrowed to the LEFT half over the branch
             * region [F+1 .. F+LEN]; elsewhere (incl. the full-width fork span F
             * and rejoin R) it is the plain full road. */
            if (branch_active && si > F_fork && si <= F_fork + TD5_TG_BRANCH_LEN) {
                const int j = si - F_fork - 1;      /* corridor step */
                if (!tg_emit_road_quad(nl, si, main_half,
                                       TD5_TG_MAIN_SHIFT(nl->v[si].width),
                                       TD5_TG_MAIN_SHIFT(nl->v[si + 1].width),
                                       0.5, &meshes))
                    ok = 0;
                /* Fill the median between the two carriageways. */
                if (ok) {
                    moff[nmesh++] = meshes.len;
                    if (!tg_emit_gore(nl, si,
                                      tg_branch_shift(j, nl->v[si].width),
                                      tg_branch_shift(j + 1, nl->v[si + 1].width),
                                      &meshes))
                        ok = 0;
                }
            } else if (!tg_emit_road_mesh(nl, si, lanes, &meshes)) {
                ok = 0;
            }
            /* Guardrails belong in THIS loop, not the box pass below: that pass
             * recovers each piece's offset by dividing the appended bytes by
             * n_added, which only holds while every piece is a same-sized box.
             * A rail prism has a different vertex count and would silently
             * corrupt those offsets. Here each offset is recorded explicitly. */
            if (ok && rails &&
                tg_span_needs_guardrail(nl, si, nspans)) {
                moff[nmesh++] = meshes.len;
                if (!tg_emit_guardrail(nl, si, &meshes)) ok = 0;
                else nrails++;
            }
        }
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;
            size_t before = meshes.len;
            int n_added = 0, k;

            if (si >= ring) continue;   /* pad + corridor carry road only */
            if (nmesh + 6 > TG_MAX_MESHES_PER_ENTRY) break;

            if (tg_span_in_tunnel(si)) {
                /* Enclosed: no buildings, they would stand inside the walls. */
                if (!tg_emit_tunnel(nl, si, &meshes, &n_added)) { ok = 0; break; }
            } else {
                if (!tg_building_for_span(nl, si, &meshes)) { ok = 0; break; }
                if (meshes.len > before) n_added = 1;
                if (!tg_emit_bridge(nl, si, &meshes, &n_added)) {
                    ok = 0; break;
                }
            }
            /* Everything emitted in this pass is a same-sized box, so each
             * piece's offset is recoverable by dividing the appended span. */
            for (k = 0; k < n_added; k++) {
                size_t sz = (meshes.len - before) / (size_t)n_added;
                moff[nmesh++] = before + (size_t)k * sz;
            }
        }

        if (ok) {
            const unsigned int hdr = (unsigned)(4 + nmesh * 4);
            tg_put_u32(&blocks[e], (unsigned)nmesh);
            for (i = 0; i < nmesh; i++)
                tg_put_u32(&blocks[e], hdr + (unsigned)moff[i]);
            if (!tg_buf_need(&blocks[e], meshes.len)) ok = 0;
            else {
                memcpy(blocks[e].b + blocks[e].len, meshes.b, meshes.len);
                blocks[e].len += meshes.len;
            }
        }
        tg_buf_free(&meshes);
    }

    if (ok) {
        /* Header: count, then (offset,size) pairs. Block 0 must begin exactly
         * at 4 + count*8 -- the strict-format-A autodetect requires it. */
        cursor = (unsigned)(4 + nentries * 8);
        tg_put_u32(out, (unsigned)nentries);
        for (e = 0; e < nentries; e++) {
            tg_put_u32(out, cursor);
            tg_put_u32(out, (unsigned)blocks[e].len);
            cursor += (unsigned)blocks[e].len;
        }
        for (e = 0; e < nentries; e++) {
            if (!tg_buf_need(out, blocks[e].len)) { ok = 0; break; }
            memcpy(out->b + out->len, blocks[e].b, blocks[e].len);
            out->len += blocks[e].len;
        }
        if (ok) {
            TD5_LOG_I(LOG_TAG, "trackgen: models = %d entries, %zu bytes "
                      "(%d road meshes)", nentries, out->len, nspans);
            /* Report coverage rather than assuming the gate is sane: a rail
             * count of 0 or of nspans both mean the curvature threshold is
             * wrong, and that is invisible without a number. */
            if (rails)
                TD5_LOG_I(LOG_TAG, "trackgen: guardrails on %d/%d spans (%d%%)",
                          nrails, nspans,
                          nspans ? (nrails * 100 / nspans) : 0);
        }
    }

    for (e = 0; e < nentries; e++) tg_buf_free(&blocks[e]);
    free(blocks);
    return ok && !out->oom;
}

/* ==========================================================================
 * TEXTURES.DAT -- texture pages for the road mesh
 *
 * Texture pages are PER-LEVEL, so a generated track must ship its own. The
 * loose textures/tex_NNN.png path is gated on g_active_td6_level > 0
 * (td5_asset.c:2864) and custom-track slots force that to 0, so it is not
 * available here -- hence the binary container, written directly like every
 * other level entry.
 *
 * Container (td5_asset.c:3064-3075):
 *   u32 page_count
 *   u32 page_offset[page_count]        -- absolute from file start
 *   per page: u8 pad[3], u8 type, i32 palette_count,
 *             u8 palette[count*3] (BGR), u8 indices[4096]   (64x64, 8-bit)
 * type: 0 opaque, 1 alpha-keyed, 2 semi-transparent, 3 additive.
 * ========================================================================== */
#define TD5_TG_TEX_DIM     64
#define TD5_TG_TEX_TEXELS  (TD5_TG_TEX_DIM * TD5_TG_TEX_DIM)
#define TD5_TG_PAL_COUNT   16

/* Page 0: asphalt with a lane line down one edge of the tile. The road mesh's
 * UVs run u = 0..lanes and tile, so a tile edge lands exactly on every lane
 * boundary -- a stripe at u=0 therefore draws lane dividers AND both road
 * edges for free, with no extra geometry. */
static void tg_emit_texture_page_asphalt(TG_Buf *out)
{
    unsigned int rng = 0x1234567u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);  /* pad[3] */
    tg_put_u8(out, 0);                                        /* type: opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Palette is BGR. 0..11 asphalt greys, 12..15 near-white for the marking. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (44 + i * 3) : (196 + (i - 12) * 12);
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (x <= 1) {
            /* Lane marking, dashed down-track so it reads as road paint. */
            idx = ((y % 24) < 16) ? (12 + (int)((rng >> 16) % 4)) : 6;
        } else {
            idx = (int)((rng >> 16) % 12);   /* asphalt grain */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 1 (+ variants): building wall -- banded masonry with lit windows. The
 * `variant` seeds the concrete tone, window tint and window rhythm so the
 * procedural streetscape mixes several facades the way the real one does. */
static void tg_emit_texture_page_wall(TG_Buf *out, int variant)
{
    unsigned int rng = 0x9E3779B9u + (unsigned)variant * 0x2545F491u;
    int wcols  = 3 + (variant % 3);      /* window bays per cell: 3..5 */
    int warm   = (variant & 1) ? 24 : 0; /* some blocks read brick-warm */
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 concrete greys, 10..12 mortar shadow, 13..15 lit windows. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 96 + i * 7;         g = 92 + i * 7;
                           r = 88 + i * 7 + warm;  }
        else if (i < 13) { b = 54;                 g = 52;
                           r = 50 + warm;          }
        else             { b = 150 + (i-13) * 30 - warm; g = 170 + (i-13) * 28;
                           r = 190 + (i-13) * 20 + warm;   }
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int cellpx = TD5_TG_TEX_DIM / (wcols + 1);   /* window cell pitch */
        int wx = (cellpx > 0) ? (x % cellpx) : x;
        int wy = y % 16;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (wy < 2 || wx < 2) {
            idx = 10 + (int)((rng >> 16) % 3);          /* storey / pier lines */
        } else if (wx >= 3 && wx <= cellpx - 3 && wy >= 5 && wy <= 12) {
            /* Window, lit or dark per cell so the facade is not uniform. */
            unsigned int cell = (unsigned)((y / 16) * 8 + (x / cellpx)) * 2654435761u;
            idx = ((cell >> 28) & 1) ? (13 + (int)((rng >> 18) % 3)) : 11;
        } else {
            idx = (int)((rng >> 16) % 10);               /* concrete */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Storefront pages: a glazed ground floor -- a coloured awning/sign band across
 * the top, big mullioned shop windows below with the odd lit pane, and a dark
 * doorway. `variant` recolours the awning (red/green/blue) so shops differ. */
static void tg_emit_texture_page_store(TG_Buf *out, int variant)
{
    static const int awn[3][3] = {           /* BGR awnings */
        { 40, 40, 170 }, { 60, 150, 60 }, { 160, 90, 40 } };
    unsigned int rng = 0x1234567u + (unsigned)variant * 0x9E3779B9u;
    int av = variant % 3, i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..5 dark glass, 6..8 frame/mullion + doorway, 9..11 awning, 12..15
     * reflection / lit-sign highlights. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 6)       { b = 46 + i * 6;  g = 40 + i * 5;  r = 34 + i * 4; }
        else if (i < 9)  { b = 26;          g = 24;          r = 22;         }
        else if (i < 12) { b = awn[av][0];  g = awn[av][1];  r = awn[av][2]; }
        else             { b = 150 + (i-12) * 24; g = 160 + (i-12) * 22;
                           r = 170 + (i-12) * 18; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 12) {
            idx = 9 + (int)((rng >> 16) % 3);            /* awning / sign band */
        } else if (y > 46 && x > 26 && x < 38) {
            idx = 6 + (int)((rng >> 16) % 3);            /* dark doorway */
        } else if ((x % 16) < 2 || (y % 20) < 2) {
            idx = 6 + (int)((rng >> 16) % 3);            /* window frame/mullion */
        } else {
            unsigned int pane = (unsigned)((y / 20) * 4 + x / 16) * 2654435761u;
            idx = ((pane >> 29) == 0) ? (12 + (int)((rng >> 18) % 4))
                                      : (int)((rng >> 16) % 6);   /* lit / glass */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 2: foliage / vegetation -- mottled greens for hedgerows and treelines.
 * Deliberately noisy rather than structured: these boxes stand in for organic
 * mass, so any regular pattern reads as wrong. */
static void tg_emit_texture_page_green(TG_Buf *out)
{
    unsigned int rng = 0x51ED2701u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. Dark shadowed foliage up to sunlit leaf, with a little earth. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 12) { b = 28 + i * 3; g = 52 + i * 9; r = 24 + i * 4; }
        else        { b = 46;         g = 58;         r = 62 + (i-12) * 6; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Clumped rather than per-texel noise: bias by a coarse cell so the
         * canopy has light and dark masses instead of uniform static. */
        {
            int x = i % TD5_TG_TEX_DIM, y = i / TD5_TG_TEX_DIM;
            unsigned int cell = (unsigned)((y / 8) * 8 + (x / 8)) * 2654435761u;
            int bias = (int)((cell >> 29) % 5);
            idx = (int)((rng >> 16) % 8) + bias;
            if (idx > 15) idx = 15;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Tree pages: an ALPHA-KEYED silhouette (type 1, index 0 = transparent key) so
 * the surround cuts out. `shape` selects the species outline so the procedural
 * fallback still mixes deciduous/conifer/palm/topiary/willow like the real set. */
static void tg_emit_texture_page_tree(TG_Buf *out, int shape)
{
    unsigned int rng = 0x2545F491u + (unsigned)shape * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key (never drawn), 1..3 trunk browns, 4..15 canopy greens
     * (olive-yellow for palm fronds). */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)                       { b = 255; g = 0;  r = 255; }
        else if (i < 4)                   { b = 30 + i * 6; g = 44 + i * 8; r = 62 + i * 10; }
        else if (shape == TG_TREE_PALM)   { b = 24 + i;     g = 70 + i * 8; r = 40 + i * 4;  }
        else                              { b = 26 + i * 2; g = 60 + i * 10; r = 22 + i * 3; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        /* y=0 is the TOP of the page; the billboard maps the base to v=1. */
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;

        switch (shape) {
        case TG_TREE_CONIFER:      /* triangle widening to the base */
            if (y >= 52) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int hw = 2 + (y * 26) / 52 - (int)((rng >> 19) % 4);
                   if (dx > -hw && dx < hw) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_PALM:         /* tall bare trunk, fronds fanning at the top */
            if (y >= 20) { if (dx > -2 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 8, rad = 20 - (ry * ry) / 6 - (int)((rng >> 19) % 4);
                   if (dx * dx < rad * rad && ((x + y) & 3) != 0)
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_TOPIARY:      /* tight round ball on a short stem */
            if (y >= 48) { if (dx > -2 && dx < 2) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 26, rad = 22 - (int)((rng >> 19) % 2);
                   if (dx * dx + ry * ry < rad * rad) idx = 4 + (int)((rng >> 16) % 8); }
            break;
        case TG_TREE_WILLOW:       /* wide drooping canopy with trailing strands */
            if (y >= 50) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int rad = 24 - (y / 4) - (int)((rng >> 19) % 3);
                   if (dx > -rad && dx < rad && (y < 30 || ((y + dx) & 1)))
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        default:                   /* TG_TREE_DECID: rough blob */
            if (y >= 44) { if (dx > -4 && dx < 4) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 22, rad = 26 - (ry * ry) / 18 - (int)((rng >> 19) % 5);
                   if (dx * dx < rad * rad) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 4: crash barrier -- galvanised steel with a dark shadow gutter along the
 * bottom and a rhythm of darker post marks.
 *
 * Reusing the building-wall page (the cheap option) was tried on paper and
 * rejected: that page is banded masonry WITH LIT WINDOWS, so a barrier drawn
 * with it reads as a low garden wall rather than as a road barrier. A page is
 * ~30 lines here, so a dedicated one is the cheaper mistake to avoid.
 *
 * The V axis runs up the barrier's face (see tg_emit_guardrail), so row 0 is
 * the bottom edge -- hence the gutter lives in the low rows, not the high ones.
 */
static void tg_emit_texture_page_rail(TG_Buf *out)
{
    unsigned int rng = 0x7F4A7C15u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 steel greys (cooler + brighter than the wall page's concrete),
     * 10..12 shadow/gutter, 13..15 specular highlight along the top rib. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 150 + i * 8; g = 148 + i * 8; r = 142 + i * 8; }
        else if (i < 13) { b = 60;          g = 58;          r = 56;          }
        else             { b = 228 + (i-13) * 8; g = 228 + (i-13) * 8;
                           r = 224 + (i-13) * 8; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 8) {
            idx = 10 + (int)((rng >> 16) % 3);        /* gutter under the rail */
        } else if (y >= 26 && y <= 34) {
            idx = 13 + (int)((rng >> 17) % 3);        /* highlight rib */
        } else if ((x % 32) < 3) {
            idx = 10 + (int)((rng >> 18) % 3);        /* post every half tile */
        } else {
            idx = (int)((rng >> 16) % 10);            /* steel */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 5: bare GROUND -- flat concrete/gravel for the terrain skirt. The point
 * is that it has NO structure: the CITY/INDUSTRIAL biomes used to floor their
 * ground with the WALL page, whose storey/window grid, stretched over the
 * undulating skirt, warped into a rippled "distorted" look filling the
 * background. Just tonal grain here -- a couple of darker gravel patches so it
 * is not a flat sheet, but no lines, so it reads as ground over any slope. */
static void tg_emit_texture_page_ground(TG_Buf *out)
{
    unsigned int rng = 0x2545F491u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..11 mid concrete greys (a touch warmer/darker than the steel rail),
     * 12..15 darker gravel/stain patches. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (104 + i * 5) : (70 - (i - 12) * 8);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));   /* faintly warm */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        /* Low-frequency patch mask so darker gravel clumps instead of speckling;
         * no axis-aligned lines, so nothing to shear over a slope. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 9) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) == 0)
            idx = 12 + (int)((rng >> 16) % 4);           /* gravel/stain */
        else
            idx = (int)((rng >> 16) % 12);               /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Emit a page from real (already palette-indexed) TD5 texture data, in the same
 * on-disk page format as the procedural emitters -- pad, opaque type, palette
 * count, BGR palette, then the 64x64 index bytes. */
static void tg_emit_real_page(TG_Buf *out, const unsigned char *pal, int paln,
                              const unsigned char *idx, int type)
{
    int i;
    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);         /* 0 opaque, 1 alpha-keyed (idx 0) */
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln * 3; i++) tg_put_u8(out, pal[i]);
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) tg_put_u8(out, idx[i]);
}

/* Opt-in: fill the road/wall/grass/ground pages with REAL TD5 textures borrowed
 * from a shipped city track (level014) instead of the procedural placeholders,
 * so the auto-track reads like an actual TD5 level. Tree + rail stay procedural
 * (the tree needs an alpha cutout the borrowed page does not carry). */
static int tg_real_textures_enabled(void)
{
    return td5_env_flag_off("TD5RE_AUTOTRACK_REAL_TEX");
}

static int tg_emit_textures(TG_Buf *out)
{
    TG_Buf pages[TD5_TG_PAGE_COUNT];
    const unsigned int count = TD5_TG_PAGE_COUNT;
    unsigned int cursor = 4 + 4 * count;
    unsigned int i;

    memset(pages, 0, sizeof(pages));

    /* ROAD and GROUND are ALWAYS procedural: the shipped city road/ground pages
     * are sandstone tan and read muddy under the auto-track, whereas the
     * procedural asphalt is grey with lane paint and the procedural ground is
     * neutral grey concrete -- both closer to what a generic street wants. Only
     * the FACADE walls (and grass) borrow real TD5 pages, which is where the
     * photographic detail actually pays off. */
    tg_emit_texture_page_asphalt(&pages[TD5_TG_PAGE_ROAD]);
    tg_emit_texture_page_ground(&pages[TD5_TG_PAGE_GROUND]);
    if (tg_real_textures_enabled()) {
        int v;
        tg_emit_real_page(&pages[TD5_TG_PAGE_WALL],
                          k_real_wall_pal[0], k_real_wall_paln[0], k_real_wall_idx[0], 0);
        for (v = 1; v < k_real_wall_count && v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1],
                              k_real_wall_pal[v], k_real_wall_paln[v], k_real_wall_idx[v], 0);
        for (v = 0; v < k_real_store_count && v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_STORE + v],
                              k_real_store_pal[v], k_real_store_paln[v], k_real_store_idx[v], 0);
        tg_emit_real_page(&pages[TD5_TG_PAGE_GREEN],
                          k_real_green_pal, k_real_green_paln, k_real_green_idx, 0);
        /* Thematic trees: alpha-keyed (type 1), index 0 transparent. */
        for (v = 0; v < TD5_TG_TREE_VARIANTS && v < k_real_tree_count; v++)
            tg_emit_real_page(&pages[tg_tree_slot(v)],
                              k_real_tree_pal[v], k_real_tree_paln[v], k_real_tree_idx[v], 1);
    } else {
        int v;
        tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL], 0);
        for (v = 1; v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1], v);
        for (v = 0; v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_texture_page_store(&pages[TD5_TG_PAGE_STORE + v], v);
        tg_emit_texture_page_green(&pages[TD5_TG_PAGE_GREEN]);
        /* Procedural trees vary by shape (deciduous/conifer). */
        for (v = 0; v < TD5_TG_TREE_VARIANTS; v++)
            tg_emit_texture_page_tree(&pages[tg_tree_slot(v)], k_tree_pages[v].shape);
    }
    tg_emit_texture_page_rail(&pages[TD5_TG_PAGE_RAIL]);
    for (i = 0; i < count; i++) {
        if (pages[i].oom) {
            for (i = 0; i < count; i++) tg_buf_free(&pages[i]);
            return 0;
        }
    }

    tg_put_u32(out, count);
    for (i = 0; i < count; i++) {           /* absolute page offsets */
        tg_put_u32(out, cursor);
        cursor += (unsigned)pages[i].len;
    }
    for (i = 0; i < count; i++) {
        if (!tg_buf_need(out, pages[i].len)) break;
        memcpy(out->b + out->len, pages[i].b, pages[i].len);
        out->len += pages[i].len;
    }
    for (i = 0; i < count; i++) tg_buf_free(&pages[i]);

    TD5_LOG_I(LOG_TAG, "trackgen: textures = %u page(s), %zu bytes",
              count, out->len);
    return !out->oom;
}

/* ---------------------------------------------------------- config ------- */
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    memset(spec, 0, sizeof(*spec));
    spec->seed          = 0;
    spec->target_spans  = 1800;
    spec->lanes         = 4;
    spec->lane_width    = TD5_TG_LANE_WIDTH;
    spec->span_length   = TD5_TG_SPAN_LENGTH;
    spec->elevation_amplitude = 6000;
    spec->circuit       = 0;    /* point-to-point: no lap wrap, cheap ribbon */
    spec->curve_safety_x100 = 180;   /* 1.80 = the Python tool's 1.5 + headroom */
    spec->max_grade_x1000   = 120;   /* 0.120 = the Python tool's max_grade */
    spec->weight[TD5_TG_STRAIGHT]  = 35;
    spec->weight[TD5_TG_CURVE]     = 40;
    spec->weight[TD5_TG_ACUTE]     = 15;
    spec->weight[TD5_TG_DUAL_LANE] = 10;
}

void td5_trackgen_apply_config(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    spec->target_spans = td5_env_int("TD5RE_AUTOTRACK_SPANS",
                                     spec->target_spans, 60, TD5_TG_MAX_SPANS);
    /* 2..4 only: shipped tracks never exceed 4 lanes, so the rail LUTs
     * (td5_track.c:1315), edge masks and suspension paths are only exercised
     * in that range. Road WIDTH still varies freely. */
    spec->lanes        = td5_env_int("TD5RE_AUTOTRACK_LANES",
                                     spec->lanes, 2, 4);
    spec->elevation_amplitude =
        td5_env_int("TD5RE_AUTOTRACK_ELEVATION",
                    spec->elevation_amplitude, 0, 40000);
    spec->weight[TD5_TG_STRAIGHT] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_STRAIGHT",
                    spec->weight[TD5_TG_STRAIGHT], 0, 100);
    spec->weight[TD5_TG_CURVE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_CURVE",
                    spec->weight[TD5_TG_CURVE], 0, 100);
    spec->weight[TD5_TG_ACUTE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_ACUTE",
                    spec->weight[TD5_TG_ACUTE], 0, 100);
    spec->weight[TD5_TG_DUAL_LANE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_DUAL",
                    spec->weight[TD5_TG_DUAL_LANE], 0, 100);
    spec->curve_safety_x100 =
        td5_env_int("TD5RE_AUTOTRACK_CURVESAFE",
                    spec->curve_safety_x100, 100, 800);
    spec->max_grade_x1000 =
        td5_env_int("TD5RE_AUTOTRACK_GRADE",
                    spec->max_grade_x1000, 0, 200);
}

/* ----------------------------------------------------------- build ------- */
int td5_trackgen_build_level(const TD5_TrackGenSpec *spec, int level_num,
                             int *out_spans)
{
    char dir[256];
    TG_NodeList nl;
    TG_Buf strip, left, right, info;
    int tally[TD5_TG_SECTION_COUNT];
    int nspans = 0, ok = 0;

    if (!spec) return 0;

    memset(&nl, 0, sizeof(nl));
    memset(&strip, 0, sizeof(strip));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&info, 0, sizeof(info));
    memset(tally, 0, sizeof(tally));

    tg_srand(spec->seed);

    snprintf(dir, sizeof(dir), "re/assets/levels/level%03d", level_num);
    _mkdir(dir);

    if (!tg_build_centerline(spec, &nl, tally)) {
        TD5_LOG_E(LOG_TAG, "trackgen: centerline build failed");
        goto done;
    }
    tg_apply_elevation(spec, &nl);

    if (td5_env_flag_off("TD5RE_AUTOTRACK_SELFCHECK")) {
        tg_selfcheck_ranges(&nl, spec->lanes,
                            td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                        TD5_TG_ORIGIN_BLOCK, 1, 20));
        s_selfcheck_regen_seed = spec->seed;   /* run after the build completes */
    }

    if (!tg_emit_strip(&nl, &strip, &nspans) || nspans < 8) {
        TD5_LOG_E(LOG_TAG, "trackgen: strip emit failed (spans=%d)", nspans);
        goto done;
    }
    /* Routes must cover exactly the ring the strip header declares. */
    /* byte0 is the lateral corridor position (0 = left rail, 255 = right).
     * Straddle the centreline symmetrically so the AI's racing line runs down
     * the middle of a road whose width varies from section to section. */
    if (!tg_emit_routes(&nl, nspans, 96,  &left) ||
        !tg_emit_routes(&nl, nspans, 160, &right)) {
        TD5_LOG_E(LOG_TAG, "trackgen: route emit failed");
        goto done;
    }
    if (!tg_emit_levelinf(spec, nspans, &info) || info.len != 100) {
        TD5_LOG_E(LOG_TAG, "trackgen: levelinf emit failed (len=%zu)", info.len);
        goto done;
    }

    if (!tg_write_file(dir, "STRIP.DAT", strip.b, strip.len) ||
        !tg_write_file(dir, "LEFT.TRK",  left.b,  left.len)  ||
        !tg_write_file(dir, "RIGHT.TRK", right.b, right.len) ||
        !tg_write_file(dir, "LEVELINF.DAT", info.b, info.len)) {
        goto done;
    }

    /* MODELS.DAT is OPT-IN (TD5RE_AUTOTRACK_SCENERY=1) and all-or-nothing:
     * its mere presence disables the procedural ribbon renderer, so if the
     * mesh bytes are wrong the road goes INVISIBLE (still drivable). Default
     * off keeps the verified ribbon path as shipped. A stale MODELS.DAT from a
     * previous opt-in run would silently keep the ribbon disabled, so remove
     * it when the knob is off. */
    {
        char models_path[320];
        snprintf(models_path, sizeof(models_path), "%s/MODELS.DAT", dir);
        /* Scenery is now DEFAULT ON (textured road, buildings, bridges,
         * biomes, tree billboards -- all verified in frame). Set
         * TD5RE_AUTOTRACK_SCENERY=0 to fall back to the untextured procedural
         * ribbon. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_SCENERY")) {
            TG_Buf models, tex;
            memset(&models, 0, sizeof(models));
            memset(&tex, 0, sizeof(tex));
            if (tg_emit_models(&nl, nspans, spec->lanes, &models))
                tg_write_file(dir, "MODELS.DAT", models.b, models.len);
            else
                TD5_LOG_W(LOG_TAG, "trackgen: models emit failed; "
                          "falling back to the ribbon renderer");
            /* Texture pages are only referenced by the mesh, so they follow
             * the same gate -- without MODELS.DAT nothing samples them. */
            if (tg_emit_textures(&tex))
                tg_write_file(dir, "TEXTURES.DAT", tex.b, tex.len);
            tg_buf_free(&models);
            tg_buf_free(&tex);
        } else {
            char tex_path[320];
            snprintf(tex_path, sizeof(tex_path), "%s/TEXTURES.DAT", dir);
            remove(models_path);
            remove(tex_path);
        }
    }

    tg_install_sky(dir, spec->seed);

    TD5_LOG_I(LOG_TAG, "trackgen: seed=%u level=%d spans=%d len=%.0f world units",
              spec->seed, level_num, nspans,
              (double)nspans * (double)spec->span_length);
    {
        int s;
        for (s = 0; s < TD5_TG_SECTION_COUNT; s++)
            TD5_LOG_I(LOG_TAG, "trackgen:   %-10s x%d (weight %d)",
                      tg_section_name((TD5_TrackGenSection)s), tally[s],
                      spec->weight[s]);
    }
    {   /* Biome layout, so a run can be checked against what is on screen. */
        int s, runs = 0;
        for (s = 0; s < nspans; s += TD5_TG_BIOME_RUN) {
            TD5_LOG_I(LOG_TAG, "trackgen:   biome span %5d.. = %s",
                      s, k_biomes[tg_biome_for_span(s)].name);
            runs++;
        }
        TD5_LOG_I(LOG_TAG, "trackgen: %d biome run(s) of %d spans",
                  runs, TD5_TG_BIOME_RUN);
    }
    ok = 1;

done:
    if (out_spans) *out_spans = nspans;
    free(nl.v);
    tg_buf_free(&strip);
    tg_buf_free(&left);
    tg_buf_free(&right);
    tg_buf_free(&info);
    return ok;
}

/* ===================== [S2] REGENERATE FOR IN-PLACE REWRITE =====================
 * Rebuild the main-road span records for `seed` and hand them back as a blob of
 * 24-byte records. Phase 2 streaming needs this so the track module can
 * overwrite a REGION of its live span array with bytes that provably match what
 * the same seed produced originally
 * (docs/plans/AUTOTRACK_STREAMING.md, stage S2).
 *
 * Ownership is deliberate: the generator produces bytes, the track module owns
 * its arrays and does the writing. This function never touches live state.
 *
 * Deterministic by construction -- tg_srand(seed) resets the private xorshift,
 * and nothing here draws from the game's rand(), so a later call with the same
 * seed reproduces the same bytes regardless of what happened in between.
 *
 * Caller frees *out_bytes. Returns 1 on success.
 */
int td5_trackgen_regenerate_main_spans(unsigned int seed,
                                      unsigned char **out_bytes,
                                      int *out_span_count)
{
    TD5_TrackGenSpec spec;
    TG_NodeList nl;
    TG_Buf spans, verts;
    int vtx_count = 0, ok = 0, block;

    if (!out_bytes || !out_span_count) return 0;
    *out_bytes = NULL;
    *out_span_count = 0;

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);
    spec.seed = seed;

    memset(&nl, 0, sizeof(nl));
    memset(&spans, 0, sizeof(spans));
    memset(&verts, 0, sizeof(verts));

    tg_srand(spec.seed);
    block = td5_env_int("TD5RE_AUTOTRACK_BLOCK", TD5_TG_ORIGIN_BLOCK, 1, 20);

    {
        int tally[TD5_TG_SECTION_COUNT];
        memset(tally, 0, sizeof(tally));
        if (tg_build_centerline(&spec, &nl, tally)) {
            tg_apply_elevation(&spec, &nl);
            if (tg_emit_span_range(&nl, 0, nl.count - 1, spec.lanes, block,
                                   &spans, &verts, &vtx_count)) {
                *out_bytes = spans.b;          /* hand the buffer over */
                *out_span_count = (int)(spans.len / 24);
                spans.b = NULL;                /* so tg_buf_free does not free it */
                spans.len = spans.cap = 0;
                ok = 1;
            }
        }
    }

    free(nl.v);
    tg_buf_free(&spans);
    tg_buf_free(&verts);

    if (ok)
        TD5_LOG_I(LOG_TAG, "trackgen: regenerated %d main span records for "
                  "seed %u (%d bytes)", *out_span_count, seed,
                  *out_span_count * 24);
    else
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate for seed %u FAILED", seed);
    return ok;
}

/* [S2 GATE, generator half] Regenerating twice from one seed must give
 * identical bytes -- otherwise an in-place rewrite could not be trusted to
 * reproduce what the track was built from. Cheap, so it runs with the same
 * TD5RE_AUTOTRACK_SELFCHECK knob as the S1 gate. */
static void tg_selfcheck_regen(unsigned int seed)
{
    unsigned char *a = NULL, *b = NULL;
    int na = 0, nb = 0;

    if (!td5_trackgen_regenerate_main_spans(seed, &a, &na) ||
        !td5_trackgen_regenerate_main_spans(seed, &b, &nb)) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen FAILED");
    } else if (na != nb || memcmp(a, b, (size_t)na * 24) != 0) {
        TD5_LOG_E(LOG_TAG, "trackgen selfcheck: regen NOT deterministic "
                  "(%d vs %d spans)", na, nb);
    } else {
        TD5_LOG_I(LOG_TAG, "trackgen selfcheck: regen PASS -- seed %u "
                  "reproduces %d identical span records on a second call",
                  seed, na);
    }
    free(a);
    free(b);
}

/* ------------------------------------------------------- lifecycle ------- */
int td5_trackgen_init(void)
{
    /* Build one track at boot so the AUTO-GENERATED entry is present (and
     * loadable) from the main menu onwards. Each race launch regenerates with
     * a fresh seed -- see td5_asset_load_level. A failure here is non-fatal:
     * the entry simply never registers and the selector skips it. */
    if (!td5_trackgen_regenerate(0))
        TD5_LOG_W(LOG_TAG, "trackgen: boot generation failed; "
                  "AUTO-GENERATED will not be selectable");
    return 1;
}

void td5_trackgen_shutdown(void)
{
    s_last_seed = 0;
}

/* -------------------------------------------------------- identity ------- */
int td5_trackgen_level_number(void) { return TD5_TG_LEVEL_NUM; }
int td5_trackgen_slot(void)         { return TD5_TG_SLOT; }

int td5_trackgen_is_auto_slot(int slot) { return slot == TD5_TG_SLOT; }

unsigned int td5_trackgen_last_seed(void) { return s_last_seed; }

int td5_trackgen_regenerate(unsigned int seed)
{
    TD5_TrackGenSpec spec;
    int spans = 0;

    td5_trackgen_default_spec(&spec);
    td5_trackgen_apply_config(&spec);

    /* TD5RE_AUTOTRACK_SEED pins the seed so two runs generate the IDENTICAL
     * track. Without this any A/B measurement compares two different random
     * roads and attributes the difference to whatever knob was changed. */
    if (seed == 0) {
        int pinned = td5_env_int("TD5RE_AUTOTRACK_SEED", 0, 0, 0x7FFFFFFF);
        seed = pinned ? (unsigned int)pinned
                      : (unsigned int)td5_plat_time_ms() * 2654435761u
                        + 0x9E3779B9u;
    }
    spec.seed = seed;

    if (!td5_trackgen_build_level(&spec, TD5_TG_LEVEL_NUM, &spans)) {
        TD5_LOG_E(LOG_TAG, "trackgen: regenerate failed; auto track unavailable");
        return 0;
    }

    s_last_seed = seed;

    if (s_selfcheck_regen_seed) {
        unsigned int sd = s_selfcheck_regen_seed;
        s_selfcheck_regen_seed = 0;
        tg_selfcheck_regen(sd);
    }

    /* Start a little way in so the grid has road behind it, and finish a few
     * spans short of the end so the walker still has road beyond the line. */
    td5_track_registry_set_auto(TD5_TG_SLOT, TD5_TG_LEVEL_NUM,
                               "AUTO-GENERATED", spec.circuit,
                               TD5_TG_GRID_SPAN,
                               spans > 8 ? spans - 4 : spans - 1);

    TD5_LOG_I(LOG_TAG, "trackgen: auto track ready (slot %d, level %d, "
              "seed %u, %d spans)", TD5_TG_SLOT, TD5_TG_LEVEL_NUM, seed, spans);
    return 1;
}
