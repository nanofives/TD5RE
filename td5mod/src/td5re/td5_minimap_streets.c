/**
 * td5_minimap_streets.c -- surrounding street network for the in-race minimap
 *                          (PORT-ONLY). Contract and rationale in the header.
 *
 * One source: the AUTO track's own street graph (td5_tg_network.c), read back
 * through td5_trackgen_street_edge_*. These edges ARE the side streets by
 * construction -- the main road is not among them. [roam SA] The GEO source
 * (every OSM way of the strip geo track's corridor, td5_geo_roads.c) went with
 * the strip geo BUILD; free roam draws its own street map (td5_roam_route_ui.c).
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
#include "deps/cjson/cJSON.h"       /* [1023 E] NETWORK.JSON fallback */

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
static int mms_push(double x0, double z0, double x1, double z1, int rank, int name_id)
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
    s->name_id = (short)((name_id >= 0 && name_id < 32767) ? name_id : -1);
    return 1;
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
            if (!mms_push(qx, qz, x, z, rank, -1)) { full = 1; break; }
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
    const int wild = td5_trackgen_is_auto_slot(slot);
    const char *slug = "";   /* [roam SA] the geo source (the strip's corridor) is gone */
    int edges;

    if (!td5_env_flag_on("TD5RE_MINIMAP_STREETS")) {
        if (s_mms.n) td5_mmstreets_invalidate();
        return 0;
    }
    /* A shipped track keeps the faithful minimap, and the signature below is
     * never even computed for it. */
    if (!wild) {
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
    edges = td5_trackgen_street_edge_count();
    if (s_mms.have &&
        s_mms.sig_slot  == slot &&
        s_mms.sig_strip == g_strip_span_base &&
        s_mms.sig_spans == g_strip_span_count &&
        s_mms.sig_edges == edges &&
        strncmp(s_mms.sig_slug, slug, sizeof s_mms.sig_slug - 1) == 0)
        return s_mms.n;

    mms_clear();
    mms_build_auto();

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

const char *td5_mmstreets_name(int id)
{
    if (id < 0 || !s_mms.n || strncmp(s_mms.source, "geo:", 4) != 0) return "";
    return td5_geo_roads_name_by_id(id);
}
