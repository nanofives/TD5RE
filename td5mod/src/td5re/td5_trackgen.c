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

#define TD5_TG_MAX_VERTICES   64000
#define TD5_TG_MAX_SPANS      3000

/* Minimum turn radius as a multiple of the road's half-width. Mirrors
 * td5_trackgen.py's CURVE_SAFETY_DEFAULT (1.5); the extra 1.2 is headroom so
 * the resampled centerline never lands exactly on the floor. */
#define TD5_TG_CURVE_SAFETY   (1.5 * 1.2)
/* Steepest allowed |dY/d(arc)|. td5_trackgen.py uses 0.12; that is far too
 * steep here, because at span_length 1500 a 0.12 grade is a 180-unit step per
 * span and the result is a series of ramps that launches the car at speed.
 * MEASURED: dead flat gives 94% wheel contact, 0.12 gives 40%. */
#define TD5_TG_MAX_GRADE      0.035

#define TD5_TG_PI 3.14159265358979323846

/* ---------------------------------------------------------------- RNG ----- */
/* Private xorshift32 -- deliberately NOT rand(). The game's rand() is the
 * MSVC-compatible one used for sim determinism and netplay lockstep
 * (td5_msvc_rand.c); drawing track geometry from it would perturb every
 * downstream random draw and break trace goldens. */
static unsigned int s_rng;

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
                radius = (width * 0.5) * TD5_TG_CURVE_SAFETY
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
                    double floor_r = (width * 0.5) * TD5_TG_CURVE_SAFETY;
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
    if (worst > TD5_TG_MAX_GRADE) {
        double k = TD5_TG_MAX_GRADE / worst;
        for (i = 0; i < nl->count; i++) nl->v[i].y *= k;
        TD5_LOG_I(LOG_TAG, "trackgen: elevation rescaled by %.3f (grade %.3f -> %.3f)",
                  k, worst, TD5_TG_MAX_GRADE);
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
            /* Left of travel is (tz, -tx). Row runs -half_width -> +half_width:
             * the reverse order renders identically (the fallback ribbon draws
             * double-sided, td5_render.c:3945) but gives a DOWNWARD surface
             * normal, so the suspension finds no ground and the car floats. */
            const double lx = n->tz, lz = -n->tx;
            int j;
            for (j = 0; j < row_pts; j++) {
                double t  = -(n->width * 0.5)
                          + (n->width * (double)j / (double)lanes);
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

    if (spans.oom || verts.oom) {
        TD5_LOG_E(LOG_TAG, "trackgen: out of memory building strip");
        ok = 0;
    }

    if (ok) {
        const int emitted = (int)(spans.len / 24);
        const unsigned int vtx_off =
            (unsigned int)(TD5_TG_SPAN_OFFSET + 24 * emitted);

        tg_put_u32(out, TD5_TG_SPAN_OFFSET);
        /* Ring length = full span count for a POINT-TO-POINT track. Shipped
         * level001 writes span_count-1 ([216,3175,...,3176]) but it is a
         * CIRCUIT, where the last span closes onto the first. Copying that
         * here made the walker wrap backward off span 0 to the ring end:
         * span_raw oscillated 0 <-> 1798 every tick, 952 times in one run. */
        tg_put_u32(out, (unsigned int)emitted);
        tg_put_u32(out, vtx_off);
        tg_put_u32(out, (unsigned int)vtx_count);
        tg_put_u32(out, (unsigned int)emitted);   /* total spans */
        tg_put_u32(out, 0);                       /* jump-entry count */
        /* Remainder of the pre-span block: no jump records, all zero. The
         * loader derives the real span count from
         * (vtx_off - span_off)/24, so this block must stay exactly
         * TD5_TG_PRE_SPAN_BYTES long. */
        tg_put_zeros(out, TD5_TG_PRE_SPAN_BYTES - 4);
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
    /* 0x54 sky_animation_index: 36 for circuits, -1 for point-to-point. */
    tg_put_u32(out, spec->circuit ? 36u : 0xFFFFFFFFu);
    tg_put_u32(out, (unsigned)nspans);          /* 0x58 total_span_count */
    tg_put_u32(out, 0);                         /* 0x5C fog_enabled */
    tg_put_u8(out, 0);                          /* 0x60 fog r */
    tg_put_u8(out, 0);                          /* 0x61 fog g */
    tg_put_u8(out, 0);                          /* 0x62 fog b */
    tg_put_u8(out, 0);                          /* 0x63 pad */
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
