/**
 * td5_minimap_streets.c -- surrounding street network for the in-race minimap
 *                          (PORT-ONLY). Contract and rationale in the header.
 *
 * Two sources, both already in memory by the time a race starts:
 *
 *   GEO   td5_geo_roads.c's parsed ROADS.JSON -- every OSM way of the real
 *         place, in the route frame. La Plata: 2 291 ways / 11 739 points, so
 *         roughly 9 400 chords. Small enough that the HUD can cull the whole
 *         array per pane per frame with four float compares each, which is why
 *         there is no spatial index here: a grid would cost more code than the
 *         scan it replaces.
 *
 *   AUTO  the generator's own street graph (td5_tg_network.c), read back
 *         through td5_trackgen_street_edge_*. These edges ARE the side streets
 *         by construction -- the main road is not among them -- so the
 *         synthetic track gets the same overlay for free.
 *
 * WHAT IS DELIBERATELY NOT FILTERED. The geo source keeps the OSM way the race
 * route itself runs along. It draws UNDER the route quads and thinner, so where
 * the two agree it is invisible, and where the route walk STOPS (the walk is
 * bounded at 48-72 quads) the real road carries on -- which is the behaviour a
 * road map has and the one a driver wants. Suppressing it would need a
 * route-proximity index and would make the map end abruptly at the walk limit.
 *
 * CORRIDOR. Points outside the route's own bbox grown by MMS_CORRIDOR are
 * dropped at build time. The minimap window is a fixed 115 * 1024 = 117 760
 * world units across (s_minimap_width is sx*115 and the scale is sx/1024, so
 * the UI scale cancels), i.e. 58 880 units of half-extent and at most 83 270 of
 * reach once the heading rotation is accounted for. The corridor is nearly
 * double that, so nothing the minimap could show is ever culled here -- it only
 * discards the rest of a city-wide fetch (La Plata's roads span 4.1M units of
 * x against the route's 2.0M).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"          /* td5_env_flag_on */
#include "td5_track.h"           /* g_strip_span_base / g_strip_span_count   */
#include "td5_trackgen.h"        /* slot predicates + street edge read-back  */
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_minimap_streets.h"

#define LOG_TAG "hud"

/* Sized for roughly 4x La Plata, matching the headroom td5_geo_roads.c gives
 * its own pool. Enforced rather than trusted: a denser place is truncated with
 * one warning, never allowed to run off the array. */
#define MMS_MAX_SEGS   40000
/* See the CORRIDOR note in the file header. */
#define MMS_CORRIDOR   150000.0
/* A chord shorter than this is a rounding artefact of the OSM geometry and can
 * never be more than a fraction of a pixel on the minimap. Dropping them keeps
 * the per-frame scan honest. */
#define MMS_MIN_CHORD  40.0

static struct {
    TD5_MMStreetSeg *seg;
    int   n, cap;
    char  source[80];
    /* Signature of the world this array describes. Rebuild when it moves. */
    int   have;
    int   sig_slot;
    const void *sig_strip;
    int   sig_spans;
    int   sig_edges;
    char  sig_slug[64];
} s_mms;

/* ------------------------------------------------------------- build ----- */

static void mms_clear(void)
{
    free(s_mms.seg);
    s_mms.seg = NULL;
    s_mms.n = s_mms.cap = 0;
    s_mms.source[0] = '\0';
}

void td5_mmstreets_invalidate(void)
{
    mms_clear();
    s_mms.have = 0;
}

static int mms_reserve(int want)
{
    TD5_MMStreetSeg *p;
    if (want > MMS_MAX_SEGS) want = MMS_MAX_SEGS;
    if (want <= s_mms.cap) return 0;        /* already at the ceiling */
    p = (TD5_MMStreetSeg *)realloc(s_mms.seg, (size_t)want * sizeof(*p));
    if (!p) return 0;
    s_mms.seg = p;
    s_mms.cap = want;
    return 1;
}

/* Bounds of the stretch of world the overlay may cover: the route when there is
 * one (the geo case), else everything (the synthetic network only ever holds
 * streets the generator attached to the road, so it needs no corridor). */
static int mms_route_bounds(double *minx, double *minz, double *maxx, double *maxz)
{
    const int n = td5_geo_route_count();
    int i, got = 0;
    for (i = 0; i < n; i++) {
        double x, z;
        if (!td5_geo_route_node(i, &x, &z, NULL)) break;
        if (!got) { *minx = *maxx = x; *minz = *maxz = z; got = 1; continue; }
        if (x < *minx) *minx = x;
        if (x > *maxx) *maxx = x;
        if (z < *minz) *minz = z;
        if (z > *maxz) *maxz = z;
    }
    if (!got) return 0;
    *minx -= MMS_CORRIDOR; *minz -= MMS_CORRIDOR;
    *maxx += MMS_CORRIDOR; *maxz += MMS_CORRIDOR;
    return 1;
}

/* 1 when the chord is long enough to be worth a quad. A walk uses this to
 * decide whether to ADVANCE its previous point: a run of sub-threshold OSM
 * points must accumulate into one drawn chord rather than vanishing one at a
 * time. */
static int mms_chord_ok(double x0, double z0, double x1, double z1)
{
    const double dx = x1 - x0, dz = z1 - z0;
    return dx * dx + dz * dz >= MMS_MIN_CHORD * MMS_MIN_CHORD;
}

/* 1 on success, 0 when the array is full (the caller must stop). */
static int mms_push(double x0, double z0, double x1, double z1, int rank)
{
    TD5_MMStreetSeg *s;
    if (s_mms.n >= s_mms.cap && !mms_reserve(s_mms.cap ? s_mms.cap * 2 : 8192)) return 0;
    s = &s_mms.seg[s_mms.n++];
    s->x0 = (float)x0; s->z0 = (float)z0;
    s->x1 = (float)x1; s->z1 = (float)z1;
    s->minx = (float)((x0 < x1) ? x0 : x1);
    s->maxx = (float)((x0 < x1) ? x1 : x0);
    s->minz = (float)((z0 < z1) ? z0 : z1);
    s->maxz = (float)((z0 < z1) ? z1 : z0);
    s->rank = (unsigned char)rank;
    return 1;
}

/* OSM highway class -> draw weight. The TD5_GEO_RC_* order is already "by
 * importance" (its header calls that the contract), so this is one threshold
 * pair rather than a table. */
static int mms_rank_for_osm(int klass)
{
    if (klass >= TD5_GEO_RC_SECONDARY) return TD5_MMS_RANK_MAJOR;
    if (klass >= TD5_GEO_RC_RESIDENTIAL) return TD5_MMS_RANK_STREET;
    return TD5_MMS_RANK_MINOR;
}

/* TG_NE_* -> draw weight. The enum is private to the generator, so the values
 * are spelled out: 0 street, 1 avenue, 2 backstreet, 3 continuation,
 * 4 country, 5 underpass, 6 bypass (td5_trackgen_internal.h). A continuation
 * carries the road's own line on, so it ranks with a street. */
static int mms_rank_for_edge(int kind)
{
    switch (kind) {
    case 1:  return TD5_MMS_RANK_MAJOR;    /* avenue       */
    case 0:                                 /* street       */
    case 3:                                 /* continuation */
    case 6:  return TD5_MMS_RANK_STREET;   /* bypass       */
    default: return TD5_MMS_RANK_MINOR;    /* back/country/underpass */
    }
}

static int mms_build_geo(const char *slug)
{
    double rminx = 0, rminz = 0, rmaxx = 0, rmaxz = 0;
    int clipped = 0, nroads, i, have_bounds, full = 0;

    if (!td5_geo_roads_sync(slug)) return 0;
    nroads = td5_geo_roads_count();
    if (nroads <= 0) return 0;

    have_bounds = mms_route_bounds(&rminx, &rminz, &rmaxx, &rmaxz);

    for (i = 0; i < nroads && !full; i++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(i);
        double px = 0, pz = 0, qx = 0, qz = 0;
        int rank, k;
        if (!r) continue;
        if (have_bounds &&
            (r->maxx < rminx || r->minx > rmaxx ||
             r->maxz < rminz || r->minz > rmaxz)) { clipped++; continue; }
        rank = mms_rank_for_osm(r->klass);
        for (k = 0; td5_geo_roads_point(r, k, &px, &pz); k++) {
            if (k == 0) { qx = px; qz = pz; continue; }
            if (!mms_chord_ok(qx, qz, px, pz)) continue;   /* keep accumulating */
            if (!mms_push(qx, qz, px, pz, rank)) {
                TD5_LOG_W(LOG_TAG, "minimap streets: hit the %d-segment ceiling "
                          "at way %d of %d; the rest of '%s' is not drawn",
                          MMS_MAX_SEGS, i, nroads, slug);
                full = 1;
                break;
            }
            qx = px; qz = pz;
        }
    }
    snprintf(s_mms.source, sizeof s_mms.source, "geo:%s", slug ? slug : "");
    TD5_LOG_I(LOG_TAG, "minimap streets: %d segment(s) from %d OSM way(s) of "
              "'%s' (%d way(s) outside the route corridor)",
              s_mms.n, nroads, slug ? slug : "", clipped);
    return s_mms.n;
}

static int mms_build_auto(void)
{
    const int ne = td5_trackgen_street_edge_count();
    int e, full = 0;
    if (ne <= 0) return 0;
    for (e = 0; e < ne && !full; e++) {
        const int np = td5_trackgen_street_edge_points(e);
        const int rank = mms_rank_for_edge(td5_trackgen_street_edge_kind(e));
        double qx = 0, qz = 0;
        int k;
        for (k = 0; k < np; k++) {
            double x, z;
            if (!td5_trackgen_street_edge_point(e, k, &x, &z)) break;
            if (k == 0) { qx = x; qz = z; continue; }
            if (!mms_chord_ok(qx, qz, x, z)) continue;
            if (!mms_push(qx, qz, x, z, rank)) { full = 1; break; }
            qx = x; qz = z;
        }
    }
    snprintf(s_mms.source, sizeof s_mms.source, "auto");
    TD5_LOG_I(LOG_TAG, "minimap streets: %d segment(s) from %d generated street "
              "edge(s)", s_mms.n, ne);
    return s_mms.n;
}

/* --------------------------------------------------------------- sync ---- */

int td5_mmstreets_sync(void)
{
    const int slot = g_td5.track_index;
    const int geo  = td5_trackgen_is_geo_slot(slot);
    const int wild = td5_trackgen_is_auto_slot(slot);
    const char *slug = (geo && td5_geo_loaded()) ? td5_geo_place_slug() : "";
    int edges;

    if (!td5_env_flag_on("TD5RE_MINIMAP_STREETS")) {
        if (s_mms.n) td5_mmstreets_invalidate();
        return 0;
    }
    /* A shipped track keeps the faithful minimap, and the signature below is
     * never even computed for it. */
    if (!geo && !wild) {
        if (s_mms.n) mms_clear();
        s_mms.have = 1;
        s_mms.sig_slot = slot;
        s_mms.sig_strip = g_strip_span_base;
        s_mms.sig_spans = g_strip_span_count;
        s_mms.sig_edges = 0;
        s_mms.sig_slug[0] = '\0';
        return 0;
    }

    /* The signature answers "is this still the same built world?". The strip
     * base pointer plus the span count move whenever a level is (re)loaded, the
     * slug moves when another place is raced, and the generated edge count
     * moves when the auto track is regenerated in place -- which the first two
     * can miss, because a regenerate can land on the same allocation with the
     * same span count. */
    edges = wild ? td5_trackgen_street_edge_count() : 0;
    if (s_mms.have &&
        s_mms.sig_slot  == slot &&
        s_mms.sig_strip == g_strip_span_base &&
        s_mms.sig_spans == g_strip_span_count &&
        s_mms.sig_edges == edges &&
        strncmp(s_mms.sig_slug, slug, sizeof s_mms.sig_slug - 1) == 0)
        return s_mms.n;

    mms_clear();
    if (geo && slug[0]) mms_build_geo(slug);
    else if (wild)      mms_build_auto();

    if (!s_mms.n && !s_mms.source[0])
        TD5_LOG_W(LOG_TAG, "minimap streets: slot %d is a generated track but no "
                  "street source is loaded; drawing the route only", slot);

    s_mms.have = 1;
    s_mms.sig_slot  = slot;
    s_mms.sig_strip = g_strip_span_base;
    s_mms.sig_spans = g_strip_span_count;
    s_mms.sig_edges = edges;
    snprintf(s_mms.sig_slug, sizeof s_mms.sig_slug, "%s", slug);
    return s_mms.n;
}

int td5_mmstreets_count(void) { return s_mms.n; }

const TD5_MMStreetSeg *td5_mmstreets_segs(void) { return s_mms.n ? s_mms.seg : NULL; }

const char *td5_mmstreets_source(void) { return s_mms.source; }
