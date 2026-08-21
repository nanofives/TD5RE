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
#define TD5_TG_PAGE_WALL   1
#define TD5_TG_PAGE_GREEN  2
#define TD5_TG_PAGE_TREE   3

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
 * Deliberately NOT a fat margin: a legal hairpin at the curvature-safety
 * floor genuinely passes within ~2*R of itself, and a generous margin would
 * reject every tight turn. Nodes within TD5_TG_ADJACENT_SKIP of each other
 * along the road are exempt (they are meant to be close); that skip is sized
 * so half a turn at the tightest legal radius clears it.
 */
#define TD5_TG_ADJACENT_SKIP 25

/* Hard cap on how far the heading may stray from the global +Z axis, radians
 * (~80 deg). This is what makes the walk NON-TRAPPING: with |heading| <= 80 deg
 * every span advances Z by at least span_length*cos(80 deg), so Z is strictly
 * increasing and two spans more than TD5_TG_ADJACENT_SKIP apart can never
 * coincide -- self-intersection becomes geometrically impossible instead of
 * merely rejected. Pure rejection sampling was tried first and traps: a
 * self-avoiding random walk in 2D paints itself into a cul-de-sac (observed:
 * 1800 spans requested, 1033 then 300 delivered).
 *
 * This still allows sharp direction changes: a section may swing from -80 to
 * +80 deg, a 160 deg switchback. What it rules out is a true hairpin that
 * doubles back down-track, which is the price of the guarantee. */
#define TD5_TG_HEADING_LIMIT 1.396

/* Global axis the walk wanders about, radians. Deliberately +X (90 deg) rather
 * than +Z (0 deg): route byte[1] encodes the ABSOLUTE 12-bit heading as
 * heading = (byte * 0x102C) >> 8 (td5_ai.c:1280), and a byte < 4 is a junction
 * sentinel rather than a heading. With the axis at 0 the commonest heading
 * (straight ahead) would encode to byte 0..3 and be read as a sentinel; at
 * 90 deg the whole +/-80 deg band maps to bytes 7..120, clear of it. */
#define TD5_TG_AXIS_HEADING (TD5_TG_PI * 0.5)

static int tg_too_close(const TG_NodeList *nl, double x, double z,
                        double width, double lane_width)
{
    const int limit = nl->count - TD5_TG_ADJACENT_SKIP;
    int i;
    for (i = 0; i < limit; i++) {
        double dx = nl->v[i].x - x;
        double dz = nl->v[i].z - z;
        double need = (nl->v[i].width + width) * 0.5 + lane_width * 0.25;
        if (dx * dx + dz * dz < need * need) return 1;
    }
    return 0;
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
                    if (heading > TD5_TG_AXIS_HEADING + TD5_TG_HEADING_LIMIT) {
                        heading = TD5_TG_AXIS_HEADING + TD5_TG_HEADING_LIMIT;
                        dir = -1;
                    } else if (heading < TD5_TG_AXIS_HEADING - TD5_TG_HEADING_LIMIT) {
                        heading = TD5_TG_AXIS_HEADING - TD5_TG_HEADING_LIMIT;
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
                if (tg_too_close(nl, x, z, width, (double)spec->lane_width)) {
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

/* Two summed sines, then a global rescale so no span exceeds MAX_GRADE.
 * Mirrors apply_road_elevation() in td5_trackgen.py. */
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

/* Lateral offset of the branch corridor at step k of TD5_TG_BRANCH_LEN: starts
 * at one road width (aligned with the outer half of the widened fork), bows out,
 * and returns, so the corridor leaves and rejoins without a large jump. */
static double tg_branch_shift(int k, double width)
{
    double f = (double)k / (double)TD5_TG_BRANCH_LEN;
    double bow = sin(f * TD5_TG_PI);            /* 0 -> 1 -> 0 */
    /* NEGATIVE: the branch takes the HIGH sub-lanes, which the widened fork
     * row places to the RIGHT of travel. Bowing left would put the corridor on
     * the opposite side from the lanes that feed it. */
    return -width * (1.0 + bow * 1.6);
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
    const int row_pts = lanes + 1;
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
    int s0;

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
    for (s0 = 0; s0 < nspans; s0 += block) {
        const int ns  = (s0 + block <= nspans) ? block : (nspans - s0);
        const int ox  = tg_round(nl->v[s0].x);
        const int oy  = tg_round(nl->v[s0].y);
        const int oz  = tg_round(nl->v[s0].z);
        const int base = vtx_count;
        int k;

        if (vtx_count + (ns + 1) * row_pts > TD5_TG_MAX_VERTICES) {
            TD5_LOG_W(LOG_TAG, "trackgen: vertex ceiling hit at span %d "
                      "(%d verts); truncating track", s0, vtx_count);
            break;
        }

        /* ns+1 rows: one per node from s0 to s0+ns inclusive, all relative to
         * this block's origin. */
        for (k = 0; k <= ns; k++) {
            const TG_Node *n = &nl->v[s0 + k];
            /* Left of travel is (tz, -tx); row runs +half_width -> -half_width.
             * NOTE: this was briefly reversed on the belief that it fixed ground
             * contact. That was based on reading actor+0x37C as a CONTACT mask
             * when it is an AIRBORNE mask (bit set = wheel airborne), so
             * "wheel_mask=0 for 947/991 ticks" actually meant all four wheels
             * GROUNDED 96% of the time. Reverted. */
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
                              "TD5_TG_ORIGIN_BLOCK", s0, dx, dy, dz);
                    ok = 0;
                }
                tg_put_u16(&verts, (unsigned)(dx & 0xFFFF));
                tg_put_u16(&verts, (unsigned)(dy & 0xFFFF));
                tg_put_u16(&verts, (unsigned)(dz & 0xFFFF));
            }
            vtx_count += row_pts;
        }

        /* Every span in the block carries the block origin and indexes two
         * consecutive rows within it. */
        for (k = 0; k < ns; k++) {
            tg_put_u8 (&spans, 1);                       /* span_type QUAD_A */
            tg_put_u8 (&spans, TD5_TG_SURFACE_ATTR);     /* surface class */
            /* Alternate surface on the two outer lanes, as shipped does. */
            tg_put_u8 (&spans, (unsigned)(1 | (1 << (lanes - 1))));
            tg_put_u8 (&spans, (unsigned)((TD5_TG_HEIGHT_NIBBLE << 4)
                                          | (lanes & 0x0F)));
            tg_put_u16(&spans, (unsigned)(base + k * row_pts));
            tg_put_u16(&spans, (unsigned)(base + (k + 1) * row_pts));
            tg_put_u16(&spans, 0xFFFF);            /* link_next = -1 */
            tg_put_u16(&spans, 0xFFFF);            /* link_prev = -1 */
            tg_put_i32(&spans, ox);
            tg_put_i32(&spans, oy);
            tg_put_i32(&spans, oz);
        }
    }

    /* ---- BRANCH (opt-in) ---- */
    {
        const int emitted_main = (int)(spans.len / 24);
        int jump_lo = 0, jump_hi = 0, jump_base = 0, have_jump = 0;

        if (ok && tg_branches_enabled() &&
            emitted_main > TD5_TG_BRANCH_FORK_SPAN + TD5_TG_BRANCH_LEN + 8) {
            const int F     = TD5_TG_BRANCH_FORK_SPAN;
            const int ring  = emitted_main;
            const int b0    = ring + 1;               /* NOT ring: see spec */
            const int main_lanes = lanes;
            const int br_lanes   = lanes;
            const int fork_lanes = main_lanes + br_lanes;
            int k;

            /* 1. Widen the approach [F-W .. F] to main+branch lanes, with
             *    dedicated rows. Without this the outer lanes never exist and
             *    sub_lane can never reach main_lanes, so the fork would be
             *    physically unreachable. */
            for (k = F - TD5_TG_BRANCH_WIDEN; k <= F; k++) {
                const TG_Node *a = &nl->v[k];
                const TG_Node *b = &nl->v[k + 1];
                /* Ramp from the plain road width up to double width. */
                double f0 = (double)(k - (F - TD5_TG_BRANCH_WIDEN))
                          / (double)TD5_TG_BRANCH_WIDEN;
                double f1 = (double)(k + 1 - (F - TD5_TG_BRANCH_WIDEN))
                          / (double)TD5_TG_BRANCH_WIDEN;
                double w0 = a->width * (1.0 + f0);
                double w1 = b->width * (1.0 + (f1 > 1.0 ? 1.0 : f1));
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi, rvi;
                /* Widen to the RIGHT of travel, NEGATIVE shift, so sub-lanes
                 * 0..main-1 stay exactly on the main road line and the extra
                 * lanes extend outward. The walker sends sub_lane <
                 * lanes(F+1) to the main continuation, so the MAIN half must
                 * be the LOW indices -- widening leftward (positive shift)
                 * moved the main lanes off their line and produced bogus wall
                 * penetrations at span F+1. */
                lvi = tg_append_row(&verts, &vtx_count, a, fork_lanes, w0,
                                    -(w0 - a->width) * 0.5, ox, oy, oz);
                rvi = tg_append_row(&verts, &vtx_count, b, fork_lanes, w1,
                                    -(w1 - b->width) * 0.5, ox, oy, oz);
                tg_patch_span(&spans, k, (k == F) ? 8 : 1, fork_lanes,
                              lvi, rvi, (k == F) ? b0 : -1, -1, ox, oy, oz);
            }

            /* 2. PAD span at index == ring, so the corridor starts at ring+1.
             *    Geometry is irrelevant; it is never linked to. */
            {
                const TG_Node *a = &nl->v[F];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, main_lanes,
                                        a->width, 0.0, ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, a, main_lanes,
                                        a->width, 0.0, ox, oy, oz);
                tg_append_span(&spans, 1, main_lanes, lvi, rvi, -1, -1,
                               ox, oy, oz);
            }

            /* 3. Corridor. Runs parallel to main spans F+1.., bowed outward,
             *    so B0 maps to main F+1 -- which is exactly what the jump
             *    record encodes. */
            for (k = 0; k < TD5_TG_BRANCH_LEN; k++) {
                const TG_Node *a = &nl->v[F + 1 + k];
                const TG_Node *b = &nl->v[F + 2 + k];
                int ox = tg_round(a->x), oy = tg_round(a->y), oz = tg_round(a->z);
                int lvi = tg_append_row(&verts, &vtx_count, a, br_lanes,
                                        a->width, tg_branch_shift(k, a->width),
                                        ox, oy, oz);
                int rvi = tg_append_row(&verts, &vtx_count, b, br_lanes,
                                        b->width, tg_branch_shift(k + 1, b->width),
                                        ox, oy, oz);
                int type = (k == 0) ? 9 : ((k == TD5_TG_BRANCH_LEN - 1) ? 10 : 1);
                int nxt  = (k == TD5_TG_BRANCH_LEN - 1) ? (F + 1 + TD5_TG_BRANCH_LEN) : -1;
                int prv  = (k == 0) ? F : -1;
                tg_append_span(&spans, type, br_lanes, lvi, rvi, nxt, prv,
                               ox, oy, oz);
            }

            jump_lo   = b0;
            jump_hi   = b0 + TD5_TG_BRANCH_LEN - 1;
            jump_base = F + 1;
            have_jump = 1;
            TD5_LOG_I(LOG_TAG, "trackgen: branch fork=%d corridor=%d..%d "
                      "base=%d (ring=%d, main=span-%d)", F, jump_lo, jump_hi,
                      jump_base, ring, jump_lo - jump_base);
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
static void tg_road_edge(const TG_NodeList *nl, int si, double f,
                         double *lx, double *ly, double *lz,
                         double *rx, double *ry, double *rz)
{
    const TG_Node *a = &nl->v[si];
    const TG_Node *b = &nl->v[si + 1];
    double x = a->x + (b->x - a->x) * f;
    double y = a->y + (b->y - a->y) * f;
    double z = a->z + (b->z - a->z) * f;
    double w = a->width + (b->width - a->width) * f;
    double tx = a->tx + (b->tx - a->tx) * f;
    double tz = a->tz + (b->tz - a->tz) * f;
    double m = sqrt(tx * tx + tz * tz);
    if (m < 1e-9) { tx = 0.0; tz = 1.0; m = 1.0; }
    tx /= m; tz /= m;
    /* Left of travel is (tz, -tx), matching the strip row order. */
    *lx = x + tz * (w * 0.5); *ly = y; *lz = z - tx * (w * 0.5);
    *rx = x - tz * (w * 0.5); *ry = y; *rz = z + tx * (w * 0.5);
}

/* One road mesh for span si, appended to blk. Returns 0 on OOM. */
static int tg_emit_road_mesh(const TG_NodeList *nl, int si, int lanes,
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
        tg_road_edge(nl, si, f0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
        tg_road_edge(nl, si, f1, &flx, &fly, &flz, &frx, &fry, &frz);
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

/* ===================== BIOMES =====================
 * A biome owns a RUN of spans and drives what stands beside the road: how
 * dense the props are, how tall, how far back, and which texture page. That is
 * what makes a stretch of city read differently from open fields without
 * needing separate emitters per biome -- every prop is still a box.
 *
 * Not yet driven by biome: the section mix (straight/curve/acute weighting).
 * The section picker runs during the centerline walk, before any of this, so
 * per-biome cornering needs the picker to know its own position first. */
typedef struct {
    const char *name;
    int    density;      /* prop if (hash>>28) <= this, so 0..15 */
    int    h_min, h_extra;
    int    gap_min, gap_extra;
    int    page;
    int    tower_mask;   /* (hash>>24)&mask == 0 -> add a tall one */
    double tile;
    int    billboard;    /* 1 = camera-facing quad (trees), 0 = box */
    int    ground_page;  /* page for the terrain slab under/around the road */
} TG_Biome;

static const TG_Biome k_biomes[] = {
    /* dense, tall, close to the road, frequent towers */
    { "CITY",       9, 1600, 5200, 2400,  3000, TD5_TG_PAGE_WALL,   3, 4500.0, 0,
      TD5_TG_PAGE_WALL },
    /* sparse low hedgerows set well back -- open horizon */
    { "FIELDS",     2, 1400, 1200, 4000, 12000, TD5_TG_PAGE_TREE,  255, 3000.0, 1,
      TD5_TG_PAGE_GREEN },
    /* dense low-to-mid greenery crowding the verge */
    { "FOREST",    11, 1800, 2200, 1600,  4000, TD5_TG_PAGE_TREE,  255, 3000.0, 1,
      TD5_TG_PAGE_GREEN },
    /* wide squat sheds, mid setback, rare towers */
    { "INDUSTRIAL", 6, 1000, 1600, 3000,  6000, TD5_TG_PAGE_WALL,  63, 6000.0, 0,
      TD5_TG_PAGE_WALL }
};
#define TD5_TG_BIOME_COUNT 4
#define TD5_TG_BIOME_RUN   150

static int tg_biome_for_span(int si)
{
    unsigned int h = (unsigned)(si / TD5_TG_BIOME_RUN) * 2654435761u;
    return (int)((h >> 27) % TD5_TG_BIOME_COUNT);
}

/* Building beside span si, or 0 if this span gets none. Deterministic from si
 * alone -- deliberately NOT the shared RNG, which has already been consumed by
 * the centerline walk, so scenery cannot perturb track shape. */
static int tg_building_for_span(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    unsigned int h = (unsigned)si * 2654435761u;
    const TG_Node *n;
    double side, gap, hx, hy, hz, lx, lz, cx, cz;

    if (si <= TD5_TG_GRID_SPAN) return 1;      /* keep the grid area clear */
    const TG_Biome *b = &k_biomes[tg_biome_for_span(si)];

    if ((int)(h >> 28) > b->density) return 1;   /* biome sets the density */

    n = &nl->v[si];
    side = ((h >> 3) & 1) ? 1.0 : -1.0;
    /* Setback is biome-driven: FIELDS pushes props right back for an open
     * horizon, CITY brings them in to frame the road. */
    gap  = (double)b->gap_min + (double)((h >> 5) % (unsigned)b->gap_extra);
    hx   = 900.0  + (double)((h >> 9) % 2600);        /* footprint */
    hz   = 900.0  + (double)((h >> 14) % 2600);
    hy   = (double)b->h_min + (double)((h >> 19) % (unsigned)b->h_extra);
    if (((h >> 24) & (unsigned)b->tower_mask) == 0)
        hy += (double)((h >> 11) % 5200);

    /* Left of travel is (tz, -tx); push out past the road edge. */
    lx = n->tz * side; lz = -n->tx * side;
    cx = n->x + lx * (n->width * 0.5 + gap + hx);
    cz = n->z + lz * (n->width * 0.5 + gap + hx);

    if (b->billboard) {
        /* Trees: one camera-facing quad standing on the ground, so it reads as
         * a tree from every angle instead of a slab with visible corners. */
        return tg_emit_billboard_mesh(blk, cx, n->y, cz,
                                      hx * 0.6, hy * 2.0, b->page);
    }
    /* Tile size is per-biome: ~4 window cells across a 4500 span reads as one
     * window per cell for masonry. */
    return tg_emit_box_mesh(blk, cx, n->y + hy, cz, hx, hy, hz,
                            n->tx, n->tz, b->page, b->tile);
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

/* Bridge: where the road runs high above the low point of the track, put a
 * deck slab just under it and a pier down to the ground. Purely cosmetic --
 * the driving surface is still the STRIP. */
static int tg_emit_bridge(const TG_NodeList *nl, int si, double ground_y,
                          TG_Buf *blk, int *added)
{
    const TG_Node *n = &nl->v[si];
    const double lift = n->y - ground_y;

    if (lift < 900.0) return 1;                     /* too low to read */

    if (!tg_emit_box_mesh(blk, n->x, n->y - 300.0, n->z,
                          n->width * 0.5 + 250.0, 200.0, 780.0,
                          n->tx, n->tz, TD5_TG_PAGE_WALL, 3000.0))
        return 0;
    (*added)++;

    if ((si & 3) == 0) {                            /* a pier every 4th span */
        double pier_h = lift * 0.5;
        if (!tg_emit_box_mesh(blk, n->x, ground_y + pier_h, n->z,
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
#define TD5_TG_GROUND_HALF_WIDTH  26000.0
#define TD5_TG_GROUND_DROP          280.0   /* below the road surface */

static int tg_emit_ground(const TG_NodeList *nl, int s0, int ns, TG_Buf *blk)
{
    /* Anchor on the middle span of the entry so the slab is centred on the
     * road it covers, and make it long enough to span the whole entry. */
    const int mid = s0 + ns / 2;
    const TG_Node *n = &nl->v[mid];
    const TG_Biome *b = &k_biomes[tg_biome_for_span(mid)];
    const double half_len = (double)ns * 0.5 * (double)TD5_TG_SPAN_LENGTH + 400.0;

    return tg_emit_box_mesh(blk, n->x, n->y - TD5_TG_GROUND_DROP, n->z,
                            TD5_TG_GROUND_HALF_WIDTH, 120.0, half_len,
                            n->tx, n->tz, b->ground_page, 9000.0);
}

static int tg_emit_models(const TG_NodeList *nl, int nspans, int lanes,
                          TG_Buf *out)
{
    const int nentries = (nspans + TD5_TG_SPANS_PER_ENTRY - 1)
                       / TD5_TG_SPANS_PER_ENTRY;
    /* Road meshes plus at most one building per span in the entry. */
    /* One ground slab, plus per span: road + building + up to 3 tunnel pieces
     * + up to 2 bridge pieces. */
    enum { TG_MAX_MESHES_PER_ENTRY = TD5_TG_SPANS_PER_ENTRY * 7 + 1 };
    TG_Buf *blocks;
    unsigned int cursor;
    int e, ok = 1;

    /* Low point of the track, so bridge piers have a ground to stand on. */
    double ground_y = nl->v[0].y;
    int gi;
    for (gi = 1; gi < nl->count; gi++)
        if (nl->v[gi].y < ground_y) ground_y = nl->v[gi].y;

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

        /* Ground slab FIRST so the road and props draw over it. Build the
         * meshes RECORDING each one's start offset -- sizes differ once
         * buildings and ground are mixed in with road quads, so offsets can no
         * longer be computed from a uniform stride. */
        moff[nmesh++] = meshes.len;
        if (!tg_emit_ground(nl, s0, ns, &meshes)) ok = 0;

        for (i = 0; i < ns && ok; i++) {
            moff[nmesh++] = meshes.len;
            if (!tg_emit_road_mesh(nl, s0 + i, lanes, &meshes)) ok = 0;
        }
        for (i = 0; i < ns && ok; i++) {
            const int si = s0 + i;
            size_t before = meshes.len;
            int n_added = 0, k;

            if (nmesh + 6 > TG_MAX_MESHES_PER_ENTRY) break;

            if (tg_span_in_tunnel(si)) {
                /* Enclosed: no buildings, they would stand inside the walls. */
                if (!tg_emit_tunnel(nl, si, &meshes, &n_added)) { ok = 0; break; }
            } else {
                if (!tg_building_for_span(nl, si, &meshes)) { ok = 0; break; }
                if (meshes.len > before) n_added = 1;
                if (!tg_emit_bridge(nl, si, ground_y, &meshes, &n_added)) {
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
        if (ok)
            TD5_LOG_I(LOG_TAG, "trackgen: models = %d entries, %zu bytes "
                      "(%d road meshes)", nentries, out->len, nspans);
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

/* Page 1: building wall -- banded masonry with lit windows. Box UVs tile every
 * 1500 world units (one lane width), so one tile reads as roughly one storey. */
static void tg_emit_texture_page_wall(TG_Buf *out)
{
    unsigned int rng = 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 concrete greys, 10..12 mortar shadow, 13..15 lit windows. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 96 + i * 7;  g = 92 + i * 7;  r = 88 + i * 7; }
        else if (i < 13) { b = 54;          g = 52;          r = 50;         }
        else             { b = 150 + (i-13) * 30; g = 170 + (i-13) * 28;
                           r = 190 + (i-13) * 20; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int wx = x % 16, wy = y % 16;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (wy < 2 || wx < 2) {
            idx = 10 + (int)((rng >> 16) % 3);          /* storey / pier lines */
        } else if (wx >= 4 && wx <= 12 && wy >= 5 && wy <= 12) {
            /* Window, lit or dark per cell so the facade is not uniform. */
            unsigned int cell = (unsigned)((y / 16) * 4 + (x / 16)) * 2654435761u;
            idx = ((cell >> 28) & 1) ? (13 + (int)((rng >> 18) % 3)) : 11;
        } else {
            idx = (int)((rng >> 16) % 10);               /* concrete */
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

/* Page 3: a single tree silhouette on an ALPHA-KEYED page (type 1), so the
 * surround cuts out and the billboard reads as a tree rather than a square.
 * Palette index 0 is reserved as the transparent key -- see the [UNCERTAIN]
 * note on tg_emit_billboard_mesh. */
static void tg_emit_texture_page_tree(TG_Buf *out)
{
    unsigned int rng = 0x2545F491u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key (never drawn), 1..3 trunk browns, 4..15 canopy greens. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)     { b = 255; g = 0;  r = 255; }   /* key: magenta */
        else if (i < 4) { b = 30 + i * 6;  g = 44 + i * 8;  r = 62 + i * 10; }
        else            { b = 26 + i * 2;  g = 60 + i * 10; r = 22 + i * 3;  }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        /* v=0 is the TOP of the page; the billboard maps the base to v=1. */
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;

        if (y >= 44) {
            /* Trunk: narrow, slightly ragged. */
            if (dx > -4 && dx < 4) idx = 1 + (int)((rng >> 17) % 3);
        } else {
            /* Canopy: a rough blob, widest around a third down, with a noisy
             * edge so the outline does not read as a circle. */
            int cy = 22;
            int ry = y - cy;
            int rad = 26 - (ry * ry) / 18 - (int)((rng >> 19) % 5);
            if (dx * dx < rad * rad) idx = 4 + (int)((rng >> 16) % 12);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

static int tg_emit_textures(TG_Buf *out)
{
    TG_Buf pages[4];
    const unsigned int count = 4;
    unsigned int cursor = 4 + 4 * count;
    unsigned int i;

    memset(pages, 0, sizeof(pages));
    tg_emit_texture_page_asphalt(&pages[TD5_TG_PAGE_ROAD]);
    tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL]);
    tg_emit_texture_page_green(&pages[TD5_TG_PAGE_GREEN]);
    tg_emit_texture_page_tree(&pages[TD5_TG_PAGE_TREE]);
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
