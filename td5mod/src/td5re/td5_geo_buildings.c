/**
 * td5_geo_buildings.c -- GEO TRACK: real OSM building footprints and area
 *                        polygons (PORT-ONLY). Contract in td5_geo_buildings.h.
 *
 * Pure reader plus polygon geometry. It writes no meshes and knows nothing
 * about trackgen: the emitters in td5_tg_city.c and td5_tg_streets.c ask it for
 * rings, heights and provenance and do all the world-building themselves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_buildings.h"
#include "td5_geo_roads.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Fallback when PLACE.JSON carries no projection.units_per_metre. Four shipped
 * constants agree on this factor (the plan's section 2); it is only a fallback
 * because the cache states its own and a future place may rescale. */
#define GEOB_UNITS_PER_M_DEFAULT 430.0

/* Refusal caps. A dense city centre holds ten thousand ways in a 2 km box (the
 * plan's section 9), and the whole point of these is that an oversized cache
 * degrades to "fewer real buildings" rather than to a failed build. */
/* [ROUND 1012 D1] RAISED for Overture. OSM alone gave La Plata 2326
 * footprints; with the Overture buildings theme conflated in (geo_fetch.py
 * conflate_extra_buildings) the same 29 km2 box holds 90840. The old 8192 cap
 * would have kept every OSM record (they come first in the file) and dropped
 * 91% of the Overture ones in FILE ORDER, i.e. most of the city. 262144 is
 * 2.9x La Plata; a TD5_GeoBuilding is ~100 bytes and the array is sized by
 * the file, not by this cap. */
#define GEOB_MAX_BUILDINGS 262144
#define GEOB_MAX_AREAS     2048
/* Ring pool hard ceiling. The pool GROWS (geob_pool_reserve) instead of being
 * malloc'd at this size up front: La Plata needs 0.5 M points after Overture,
 * and a 4 M x 16-byte pool on every geo load would be 64 MB for nothing. */
#define GEOB_MAX_POINTS    (4 * 1024 * 1024)
#define GEOB_POOL_INITIAL  65536
/* BUILDINGS.JSON was 1.5 MB at La Plata with OSM only. With Overture it is
 * tens of MB written compact by geo_fetch, and the derived _route/ copy is
 * re-printed FORMATTED by td5_geo_route.c (cJSON_Print, 17-digit doubles), so
 * the cap has to clear several times that. Still refuses a file that is not
 * what we think it is. */
#define GEOB_MAX_JSON      (192 * 1024 * 1024)
/* [ROUND 1013 F1] Named plaza rings kept, and hull vertices per ring. La Plata
 * has 5 rings; the largest group's hull is a few dozen points. A ring whose hull
 * would not fit is skipped and logged, never truncated into a wrong shape. */
#define GEOB_PRING_MAX     32
#define GEOB_PRING_PTS     128
/* A footprint standing inside a plaza is kept only if it is a landmark or a
 * small OSM-tagged building (a kiosk, a monument base, a bandstand): 60 m2. */
#define GEOB_PLAZA_KEEP_M2 60.0

static struct {
    int    loaded;
    char   slug[64];
    double units_per_m;
    double storey_m;

    TD5_GeoBuilding *b;
    int    nb;
    TD5_GeoArea     *a;
    int    na;

    double *px, *pz;          /* shared ring point pool */
    int    npt;
    int    pcap;              /* pool capacity, grows to GEOB_MAX_POINTS */

    int   *b_head, *a_head;   /* per-span chain heads, bound_spans entries */
    int   *b_next, *a_next;
    int    bound_spans;       /* route nodes the chains cover, 0 = unbound  */
    int    b_bound, b_far, a_bound, a_far;

    int    dec_polys, dec_points;   /* decimation cost */

    /* [ROUND 1013 F1] named plaza RINGS, as convex hulls. See
     * td5_geob_in_plaza_ring. 0 rings on a place whose plazas are all mapped
     * as polygons, and on every synthetic build (nothing here is loaded). */
    int    n_pring;
    int    pring_on;                /* TD5RE_GEO_PLAZA_RING, latched at load */
    struct {
        double cx, cz, r;           /* centroid + max point distance        */
        int    n;                   /* hull vertices                        */
        double hx[GEOB_PRING_PTS], hz[GEOB_PRING_PTS];
        char   name[48];
    } pring[GEOB_PRING_MAX];
    int    veto_n, veto_small, veto_landmark;
} s_gb;

/* ----------------------------------------------------------------- io ------ */

static char *geob_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GEOB_MAX_JSON) {
        TD5_LOG_W(LOG_TAG, "geob: %s is %lld bytes, outside 1..%d; skipped",
                  path, (long long)n, GEOB_MAX_JSON);
        td5_plat_file_close(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) {
        td5_plat_file_close(f);
        return NULL;
    }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf);
        td5_plat_file_close(f);
        return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

/* --------------------------------------------------------------- geometry -- */

double td5_geob_ring_area(const double *x, const double *z, int n)
{
    double a = 0.0;
    int i, j;
    if (n < 3) return 0.0;
    for (i = 0, j = n - 1; i < n; j = i++)
        a += x[j] * z[i] - x[i] * z[j];
    return a * 0.5;
}

int td5_geob_point_in_ring(const double *x, const double *z, int n,
                           double px, double pz)
{
    int i, j, in = 0;
    if (n < 3) return 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        if (((z[i] > pz) != (z[j] > pz))
            && px < (x[j] - x[i]) * (pz - z[i]) / (z[j] - z[i]) + x[i])
            in = !in;
    }
    return in;
}

/* Do the open segments (a0,a1) and (b0,b1) properly cross? Sign-of-area test,
 * strict on both sides, so a shared endpoint (which every adjacent pair of
 * ring edges has) is NOT a crossing. */
static int geob_seg_cross(const double *x, const double *z,
                          int a0, int a1, int b0, int b1)
{
    const double d1 = (x[a1] - x[a0]) * (z[b0] - z[a0])
                    - (z[a1] - z[a0]) * (x[b0] - x[a0]);
    const double d2 = (x[a1] - x[a0]) * (z[b1] - z[a0])
                    - (z[a1] - z[a0]) * (x[b1] - x[a0]);
    const double d3 = (x[b1] - x[b0]) * (z[a0] - z[b0])
                    - (z[b1] - z[b0]) * (x[a0] - x[b0]);
    const double d4 = (x[b1] - x[b0]) * (z[a1] - z[b0])
                    - (z[b1] - z[b0]) * (x[a1] - x[b0]);
    return ((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0));
}

/* ~0.5 m2 at 430 units/m. Below this a "footprint" is a traced line, not a
 * building, and the emitters have nothing to extrude. */
#define GEOB_RING_MIN_AREA  1.0e5

int td5_geob_ring_simple(const double *x, const double *z, int n)
{
    int i, j;
    if (n < 3) return 0;
    if (fabs(td5_geob_ring_area(x, z, n)) < GEOB_RING_MIN_AREA) return 0;
    for (i = 0; i < n; i++) {
        const int i1 = (i + 1) % n;
        for (j = i + 1; j < n; j++) {
            const int j1 = (j + 1) % n;
            if (j == i || j1 == i || j == i1) continue;   /* shares an end */
            if (geob_seg_cross(x, z, i, i1, j, j1)) return 0;
        }
    }
    return 1;
}

/* Is p strictly inside triangle (a,b,c), for a CCW triangle? */
static int geob_in_tri(double ax, double az, double bx, double bz,
                       double cx, double cz, double px, double pz)
{
    const double d1 = (bx - ax) * (pz - az) - (bz - az) * (px - ax);
    const double d2 = (cx - bx) * (pz - bz) - (cz - bz) * (px - bx);
    const double d3 = (ax - cx) * (pz - cz) - (az - cz) * (px - cx);
    return d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0;
}

/* Ear clipping. O(n^2) on rings of at most TD5_GEOB_RING_MAX points, so the
 * worst case here is 48*48 -- cheaper than the mesh bytes it produces.
 *
 * WHY NOT A CENTROID FAN. A fan is two lines of code and is wrong for exactly
 * the footprints that matter: an L-shaped block or a cruciform church has a
 * centroid OUTSIDE its own outline, so fan triangles spill past the walls and
 * the roof reads as a slab hanging over the street. Ear clipping keeps every
 * triangle inside the ring by construction. */
int td5_geob_triangulate(const double *x, const double *z, int n,
                         int *tri, int maxtri)
{
    int idx[TD5_GEOB_RING_MAX];
    int m = n, ntri = 0, guard, i;

    if (n < 3 || n > TD5_GEOB_RING_MAX || maxtri < n - 2) return 0;
    /* Work on a CCW copy: the ear test below assumes positive orientation. */
    if (td5_geob_ring_area(x, z, n) < 0.0)
        for (i = 0; i < n; i++) idx[i] = n - 1 - i;
    else
        for (i = 0; i < n; i++) idx[i] = i;

    /* Each pass must remove at least one ear; `guard` bounds the loop so a
     * self-intersecting ring (OSM has them) stops rather than spins. */
    guard = 2 * n;
    while (m > 3 && guard-- > 0) {
        int clipped = 0;
        for (i = 0; i < m; i++) {
            const int ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
            const double ax = x[ia], az = z[ia];
            const double bx = x[ib], bz = z[ib];
            const double cx = x[ic], cz = z[ic];
            const double cross = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            int k, ok = 1;
            if (cross <= 0.0) continue;            /* reflex or degenerate */
            for (k = 0; k < m && ok; k++) {
                const int ip = idx[k];
                if (ip == ia || ip == ib || ip == ic) continue;
                if (geob_in_tri(ax, az, bx, bz, cx, cz, x[ip], z[ip])) ok = 0;
            }
            if (!ok) continue;
            tri[ntri * 3 + 0] = ia;
            tri[ntri * 3 + 1] = ib;
            tri[ntri * 3 + 2] = ic;
            ntri++;
            for (k = i; k < m - 1; k++) idx[k] = idx[k + 1];
            m--;
            clipped = 1;
            break;
        }
        if (!clipped) break;    /* no ear found: ring is not simple */
    }
    if (m == 3 && ntri + 1 <= maxtri) {
        tri[ntri * 3 + 0] = idx[0];
        tri[ntri * 3 + 1] = idx[1];
        tri[ntri * 3 + 2] = idx[2];
        ntri++;
    }
    return ntri;
}

/* ------------------------------------------------------------------ parse -- */

/* The three cJSON field accessors. Up here rather than beside the loaders
 * because the tag rules below read them. */
static const char *geob_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static double geob_num(const cJSON *o, const char *key, double def)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : def;
}

static int geob_true(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return v && cJSON_IsTrue(v);
}

static unsigned int geob_id_hash(double id)
{
    /* The way id arrives as a JSON number, so it can exceed int range. Fold it
     * through a double->unsigned pair rather than truncating, so two ids that
     * differ only above 2^32 still pick different pages. */
    const double a = floor(fabs(id));
    unsigned int lo = (unsigned int)fmod(a, 4294967296.0);
    unsigned int hi = (unsigned int)fmod(floor(a / 4294967296.0), 4294967296.0);
    unsigned int h = lo * 2654435761u ^ (hi + 0x9E3779B9u);
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
}

/* roof:shape -> the silhouette the emitter builds.
 *
 * The shapes below are the ones the OSM wiki lists and the ones that actually
 * occur; the DEFAULT is still APEX, because an untranslated value ("cone",
 * "quadruple_saltbox", a typo) is far more likely to be some kind of slope
 * than to be flat, and a pyramid is the silhouette that cannot be wrong in a
 * way that pokes through a wall.
 *
 * `gabled_with_*` is matched by PREFIX: the wiki sanctions a family of
 * gabled_with_<something> values and they are all ridged. */
static int geob_roof_of(const char *s)
{
    if (!s || !s[0]) return TD5_GEOB_ROOF_NONE;
    if (!strcmp(s, "flat"))                            return TD5_GEOB_ROOF_FLAT;
    if (!strcmp(s, "mansard") || !strcmp(s, "gambrel"))
        return TD5_GEOB_ROOF_MANSARD;
    if (!strcmp(s, "gabled") || !strcmp(s, "half-hipped")
        || !strcmp(s, "side_hipped") || !strcmp(s, "saltbox")
        || !strncmp(s, "gabled_with", 11))             return TD5_GEOB_ROOF_GABLED;
    if (!strcmp(s, "hipped") || !strcmp(s, "hipped_and_gabled"))
        return TD5_GEOB_ROOF_HIPPED;
    if (!strcmp(s, "skillion") || !strcmp(s, "lean_to")
        || !strcmp(s, "shed") || !strcmp(s, "monopitch"))
        return TD5_GEOB_ROOF_SKILLION;
    /* pyramidal, dome, onion, round, cone, sphere and anything unrecognised. */
    return TD5_GEOB_ROOF_APEX;
}

/* A number that may have arrived as a JSON STRING.
 *
 * Overpass hands every tag back as a string and geo_fetch stores height,
 * min_height and roof:height RAW (re/tools/geo_fetch.py, the buildings
 * branch), so `geob_num` -- which only accepts cJSON numbers -- read 0 for
 * every one of them. MEASURED on the La Plata cache: min_height_m is absent
 * on all 2047 footprints and roof_height_m is present on exactly one, as the
 * string "5". Accepting a leading numeric prefix also absorbs the "12 m" and
 * "3.5m" spellings OSM allows. */
static double geob_num_tag(const cJSON *o, const char *key, double def)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!v) return def;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end != v->valuestring) return d;
    }
    return def;
}

/* A building=* value that names a landmark ON ITS OWN, with or without a
 * name tag.
 *
 * geo_fetch's rule used to be `tourism or historic or (name and building in
 * {cathedral, church, stadium, museum, train_station, civic, public})`.
 * MEASURED on the La Plata cache: 10 of 2047 footprints passed it and only 2
 * came within 100 m of the route, so the whole landmark path shipped
 * exercised twice. These classes come off the `class` field that has ALWAYS
 * been in BUILDINGS.JSON, so the rule works on a cache already on disk --
 * which is why it lives here as well as in geo_fetch.
 *
 * The set is deliberately the UNMISTAKABLE classes. `university`, `school`,
 * `hospital`, `office` and `retail` are ordinary urban fabric at La Plata's
 * scale (29 universities and 13 schools in this one cache) and promoting them
 * would hand a quarter of the city the landmark treatment.
 *
 * THE GAP RECORDED HERE IS NOW CLOSED (2026-10-07). The tags that caught the
 * rest -- amenity=place_of_worship / theatre / police, office=government,
 * government=* -- were not carried into BUILDINGS.JSON at all, so no C-side
 * rule could see them: 9 further landmark-worthy objects on the La Plata
 * route. geo_fetch.py now keeps them (docs/plans/GEO_TAG_AUDIT.md) and
 * decides `landmark` from the full table itself, so for a cache at
 * `tag_schema` 2 this function is a belt-and-braces agreement rather than the
 * only rule. It still matters for an OLD cache, and the tag rule below still
 * matters for a cache land_relabel.py has not been run over. */
static int geob_class_is_landmark(const char *c)
{
    static const char *const k[] = {
        "cathedral", "church", "chapel", "basilica", "mosque", "synagogue",
        "temple", "monastery", "shrine", "stadium", "museum", "palace",
        "castle", "monument", "memorial", "train_station", "courthouse",
        "townhall", "government", "civic", "public", "theatre", "opera_house",
        NULL
    };
    int i;
    if (!c || !c[0]) return 0;
    for (i = 0; k[i]; i++) if (!strcmp(c, k[i])) return 1;
    return 0;
}

const char *td5_geob_lmsrc_name(int src)
{
    static const char *const k[TD5_GEOB_LMSRC_COUNT] = {
        "none", "flag", "building", "government", "office", "amenity",
        "historic", "heritage", "man_made", "tourism"
    };
    return (src >= 0 && src < TD5_GEOB_LMSRC_COUNT) ? k[src] : "?";
}

/* Does one of `vals` equal `v`? NULL-terminated list, NULL/empty v is no. */
static int geob_in_set(const char *v, const char *const *vals)
{
    int i;
    if (!v || !v[0]) return 0;
    for (i = 0; vals[i]; i++) if (!strcmp(v, vals[i])) return 1;
    return 0;
}

/* THE TAG RULE, over the civic keys geo_fetch now promotes to fields.
 *
 * Mirrors geo_fetch.py's LANDMARK_RULE, in the SAME attribution order (most
 * specific statement of what the structure IS first), so the key this returns
 * and the key `landmark_src` names agree. It is evaluated here as well as
 * there for two reasons that are not redundancy:
 *
 *  - a cache that `land_relabel.py` upgraded carries the tags and a stale
 *    `landmark` bool it cannot recompute for every rule;
 *  - it is the ONE place a reader can be A/B'd. TD5RE_GEO_LM_TAGS=0 turns the
 *    tag promotion off and leaves the cache's own bool plus the class rule,
 *    which is what the pre-2026-10-07 reader saw -- so the La Plata MODELS.DAT
 *    delta can be attributed to the tags instead of asserted.
 *
 * Returns TD5_GEOB_LMSRC_NONE when nothing fires. The tourism exclusions are
 * geo_fetch's TOURISM_NOT_LANDMARK: a hotel carries `tourism=*` and is
 * ordinary street frontage. */
static int geob_tag_landmark_src(const cJSON *e)
{
    static const char *const k_amenity[] = {
        "place_of_worship", "theatre", "townhall", "courthouse", "arts_centre",
        "police", "fire_station", "embassy", "casino", "cinema",
        "conference_centre", "exhibition_centre", "monastery",
        "public_building", NULL
    };
    static const char *const k_office[]   = { "government", "diplomatic", NULL };
    static const char *const k_man_made[] = { "tower", "lighthouse", "obelisk",
                                              "water_tower", "campanile", NULL };
    static const char *const k_tourism[]  = { "attraction", "museum", "gallery",
                                              "artwork", "viewpoint",
                                              "theme_park", "aquarium", "zoo",
                                              "monument", NULL };
    const char *v;

    if (geob_class_is_landmark(geob_str(e, "class")))
        return TD5_GEOB_LMSRC_BUILDING;
    v = geob_str(e, "government");
    if (v && v[0]) return TD5_GEOB_LMSRC_GOVERNMENT;   /* any value */
    if (geob_in_set(geob_str(e, "office"), k_office))
        return TD5_GEOB_LMSRC_OFFICE;
    if (geob_in_set(geob_str(e, "amenity"), k_amenity))
        return TD5_GEOB_LMSRC_AMENITY;
    v = geob_str(e, "historic");
    if (v && v[0]) return TD5_GEOB_LMSRC_HISTORIC;     /* any value */
    v = geob_str(e, "heritage");
    if (v && v[0]) return TD5_GEOB_LMSRC_HERITAGE;     /* any value */
    if (geob_in_set(geob_str(e, "man_made"), k_man_made))
        return TD5_GEOB_LMSRC_MANMADE;
    if (geob_in_set(geob_str(e, "tourism"), k_tourism))
        return TD5_GEOB_LMSRC_TOURISM;
    return TD5_GEOB_LMSRC_NONE;
}

/* THE PRE-2026-10-07 geo_fetch RULE, recomputed from the record.
 *
 * It was, verbatim:
 *   tourism or historic or (name and building in {cathedral, church, stadium,
 *                           museum, train_station, civic, public})
 * -- a BLANKET `tourism`, which is why a boutique hostel was a La Plata
 * landmark, and a NAME requirement on a narrow building set, which is why an
 * unnamed cathedral was not.
 *
 * Every field it reads is in BUILDINGS.JSON at tag_schema 2 (`tourism` and
 * `historic` are promoted fields now, `name` and `class` always were), so the
 * old answer is exactly reconstructible on the NEW cache. That is what makes
 * TD5RE_GEO_LM_TAGS=0 an A/B of one cache rather than a comparison of two. */
static int geob_legacy_landmark(const cJSON *e)
{
    static const char *const k_narrow[] = {
        "cathedral", "church", "stadium", "museum", "train_station",
        "civic", "public", NULL
    };
    const char *nm;
    const char *v = geob_str(e, "tourism");
    if (v && v[0]) return 1;
    v = geob_str(e, "historic");
    if (v && v[0]) return 1;
    nm = geob_str(e, "name");
    return (nm && nm[0] && geob_in_set(geob_str(e, "class"), k_narrow)) ? 1 : 0;
}

/* The KEY of geo_fetch's "key=value" `landmark_src`, as a TD5_GEOB_LMSRC_*.
 * The value half is deliberately not kept: the emitters branch on nothing
 * finer than "is this a landmark", and a per-value code would be a table to
 * maintain in two languages for a log line. */
static int geob_lmsrc_of_string(const char *s)
{
    static const struct { const char *key; int src; } k[] = {
        { "building",   TD5_GEOB_LMSRC_BUILDING   },
        { "government", TD5_GEOB_LMSRC_GOVERNMENT },
        { "office",     TD5_GEOB_LMSRC_OFFICE     },
        { "amenity",    TD5_GEOB_LMSRC_AMENITY    },
        { "historic",   TD5_GEOB_LMSRC_HISTORIC   },
        { "heritage",   TD5_GEOB_LMSRC_HERITAGE   },
        { "man_made",   TD5_GEOB_LMSRC_MANMADE    },
        { "tourism",    TD5_GEOB_LMSRC_TOURISM    }
    };
    const int n = (int)(sizeof(k) / sizeof(k[0]));
    size_t len;
    const char *eq;
    int i;

    if (!s || !s[0]) return TD5_GEOB_LMSRC_NONE;
    eq = strchr(s, '=');
    len = eq ? (size_t)(eq - s) : strlen(s);
    for (i = 0; i < n; i++)
        if (strlen(k[i].key) == len && !strncmp(s, k[i].key, len))
            return k[i].src;
    return TD5_GEOB_LMSRC_NONE;
}

/* AREAS.JSON kind -> what the plaza emitter distinguishes.
 *
 * WIDENED 2026-10-07 with the values geo_fetch's whitelist used to refuse
 * outright (docs/plans/GEO_TAG_AUDIT.md): La Plata carries leisure=track x7,
 * swimming_pool x2, sports_centre x2, stadium, bleachers, fitness_station and
 * landuse=education x4 / railway x3 / plant_nursery, and they were the whole
 * of the cache's `ignored` count. `natural` is read as well, which it never
 * was -- the branch only ever consulted it for water.
 *
 * A running TRACK, a STADIUM bowl and a sports CENTRE ground are flat laid
 * surfaces, which is what PITCH means here, not lawn. `education`,
 * `institutional` and `railway` land on OTHER on purpose: a campus block and a
 * rail yard are BUILT ground, and laying a lawn over a mapped campus would
 * carpet it -- the same argument the header records for residential/retail. */
static int geob_area_kind_of(const char *leisure, const char *landuse,
                             const char *natural, int wide)
{
    if (leisure) {
        if (!strcmp(leisure, "park"))           return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "common"))         return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "pitch"))          return TD5_GEOA_KIND_PITCH;
        if (!strcmp(leisure, "playground"))     return TD5_GEOA_KIND_PLAY;
        if (!strcmp(leisure, "garden"))         return TD5_GEOA_KIND_PARK;
    }
    if (landuse) {
        if (!strcmp(landuse, "grass"))         return TD5_GEOA_KIND_GRASS;
        if (!strcmp(landuse, "village_green")) return TD5_GEOA_KIND_PARK;
        if (!strcmp(landuse, "forest"))        return TD5_GEOA_KIND_FOREST;
        if (!strcmp(landuse, "meadow"))        return TD5_GEOA_KIND_GRASS;
    }
    /* Everything from here down is the 2026-10-07 widening, so
     * TD5RE_GEO_AREA_TAGS=0 reproduces the old classification exactly on the
     * new cache -- the same A/B shape as TD5RE_GEO_LM_TAGS. An area the old
     * reader never saw at all (the whitelist refused it upstream) still
     * arrives in AREAS.JSON and still lands on OTHER here, which is what the
     * old reader would have done with it. */
    if (!wide) return TD5_GEOA_KIND_OTHER;
    if (leisure) {
        if (!strcmp(leisure, "nature_reserve")) return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "picnic_site"))    return TD5_GEOA_KIND_PARK;
        if (!strcmp(leisure, "golf_course"))    return TD5_GEOA_KIND_GRASS;
        /* A running TRACK, a STADIUM bowl and a sports CENTRE ground are flat
         * laid surfaces, which is what PITCH means here -- not lawn. */
        if (!strcmp(leisure, "track"))          return TD5_GEOA_KIND_PITCH;
        if (!strcmp(leisure, "stadium"))        return TD5_GEOA_KIND_PITCH;
        if (!strcmp(leisure, "sports_centre"))  return TD5_GEOA_KIND_PITCH;
    }
    if (landuse) {
        if (!strcmp(landuse, "grassland"))         return TD5_GEOA_KIND_GRASS;
        if (!strcmp(landuse, "recreation_ground")) return TD5_GEOA_KIND_PARK;
        if (!strcmp(landuse, "cemetery"))          return TD5_GEOA_KIND_GRASS;
    }
    /* `natural` was NEVER read by this layer: geo_fetch only ever consulted it
     * for water, so `natural=wood` could not become a forest even though
     * _OSM_TO_COVER had a row for it. */
    if (natural) {
        if (!strcmp(natural, "wood"))      return TD5_GEOA_KIND_FOREST;
        if (!strcmp(natural, "tree_row"))  return TD5_GEOA_KIND_FOREST;
        if (!strcmp(natural, "grassland")) return TD5_GEOA_KIND_GRASS;
        if (!strcmp(natural, "heath"))     return TD5_GEOA_KIND_GRASS;
        if (!strcmp(natural, "scrub"))     return TD5_GEOA_KIND_GRASS;
    }
    return TD5_GEOA_KIND_OTHER;
}

/* Make room for `need` pool points. Doubling, so a 90k-footprint load costs
 * a handful of reallocs. Returns 0 (ring rejected, the pool stays valid) at
 * the hard ceiling or on allocation failure. */
static int geob_pool_reserve(int need)
{
    int cap;
    double *nx, *nz;
    if (need <= s_gb.pcap) return 1;
    if (need > GEOB_MAX_POINTS) return 0;
    cap = s_gb.pcap > 0 ? s_gb.pcap : GEOB_POOL_INITIAL;
    while (cap < need)
        cap = (cap > GEOB_MAX_POINTS / 2) ? GEOB_MAX_POINTS : cap * 2;
    nx = (double *)realloc(s_gb.px, (size_t)cap * sizeof(double));
    if (!nx) return 0;
    s_gb.px = nx;
    nz = (double *)realloc(s_gb.pz, (size_t)cap * sizeof(double));
    if (!nz) return 0;
    s_gb.pz = nz;
    s_gb.pcap = cap;
    return 1;
}

/* Copy one `points` array into the shared pool as an OPEN, CCW ring.
 * Returns the number of pool points written (0 = rejected). */
static int geob_push_ring(const cJSON *pts, int *out_first,
                          double *out_cx, double *out_cz, double *out_r)
{
    double rx[TD5_GEOB_RING_MAX], rz[TD5_GEOB_RING_MAX];
    int raw, n = 0, i, stride;
    double cx = 0.0, cz = 0.0, r = 0.0;

    if (!pts || !cJSON_IsArray(pts)) return 0;
    raw = cJSON_GetArraySize(pts);
    /* geo_fetch closes every ring, so the last point repeats the first. */
    if (raw >= 2) {
        const cJSON *p0 = cJSON_GetArrayItem(pts, 0);
        const cJSON *pl = cJSON_GetArrayItem(pts, raw - 1);
        if (geob_num(p0, "x", 0.0) == geob_num(pl, "x", 1.0)
            && geob_num(p0, "z", 0.0) == geob_num(pl, "z", 1.0))
            raw--;
    }
    if (raw < 3) return 0;

    /* DECIMATE rather than truncate: a uniform stride keeps the silhouette and
     * only loses detail, where cutting the tail would leave an open ring that
     * closes across the middle of the building. */
    stride = (raw + TD5_GEOB_RING_MAX - 1) / TD5_GEOB_RING_MAX;
    if (stride < 1) stride = 1;
    if (stride > 1) { s_gb.dec_polys++; s_gb.dec_points += raw - raw / stride; }
    for (i = 0; i < raw && n < TD5_GEOB_RING_MAX; i += stride) {
        const cJSON *p = cJSON_GetArrayItem(pts, i);
        const cJSON *vx = p ? cJSON_GetObjectItem(p, "x") : NULL;
        const cJSON *vz = p ? cJSON_GetObjectItem(p, "z") : NULL;
        if (!vx || !vz || !cJSON_IsNumber(vx) || !cJSON_IsNumber(vz)) return 0;
        rx[n] = vx->valuedouble;
        rz[n] = vz->valuedouble;
        n++;
    }
    if (n < 3) return 0;
    if (!geob_pool_reserve(s_gb.npt + n)) return 0;

    /* One winding for every ring, so the triangulator and the wall loop can
     * both assume it instead of each testing. */
    if (td5_geob_ring_area(rx, rz, n) < 0.0) {
        for (i = 0; i < n / 2; i++) {
            double t = rx[i]; rx[i] = rx[n - 1 - i]; rx[n - 1 - i] = t;
            t = rz[i]; rz[i] = rz[n - 1 - i]; rz[n - 1 - i] = t;
        }
    }
    for (i = 0; i < n; i++) { cx += rx[i]; cz += rz[i]; }
    cx /= (double)n; cz /= (double)n;
    for (i = 0; i < n; i++) {
        const double d = hypot(rx[i] - cx, rz[i] - cz);
        if (d > r) r = d;
    }

    *out_first = s_gb.npt;
    for (i = 0; i < n; i++) {
        s_gb.px[s_gb.npt] = rx[i];
        s_gb.pz[s_gb.npt] = rz[i];
        s_gb.npt++;
    }
    *out_cx = cx; *out_cz = cz; *out_r = r;
    return n;
}

static int geob_load_place_scale(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root;

    s_gb.units_per_m = GEOB_UNITS_PER_M_DEFAULT;
    td5_geo_place_path(path, sizeof(path), slug, "PLACE.JSON");
    json = geob_slurp(path);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    {
        const cJSON *pr = cJSON_GetObjectItem(root, "projection");
        const double u = geob_num(pr, "units_per_metre", 0.0);
        if (u > 1.0) s_gb.units_per_m = u;
    }
    cJSON_Delete(root);
    return 1;
}

static int geob_load_buildings(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    /* Read ONCE for the whole file, not per footprint: an env knob is fixed
     * for a process run and this loop is 2047 iterations at La Plata. */
    const int tag_lm = td5_env_flag_on("TD5RE_GEO_LM_TAGS");
    int n, i;
    const cJSON *e_next;

    td5_geo_place_path(path, sizeof(path), slug, "BUILDINGS.JSON");
    json = geob_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no readable BUILDINGS.JSON; the "
                  "procedural facades stand everywhere", slug);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geob: BUILDINGS.JSON is not valid JSON");
        return 0;
    }
    s_gb.storey_m = geob_num(root, "storey_height_m", 3.0);
    arr = cJSON_GetObjectItem(root, "buildings");
    if (!arr || !cJSON_IsArray(arr)) {
        TD5_LOG_E(LOG_TAG, "geob: BUILDINGS.JSON has no buildings[]");
        cJSON_Delete(root);
        return 0;
    }
    n = cJSON_GetArraySize(arr);
    if (n > GEOB_MAX_BUILDINGS) {
        TD5_LOG_W(LOG_TAG, "geob: %d buildings, capped at %d (the rest are "
                  "dropped; the procedural filler covers them)",
                  n, GEOB_MAX_BUILDINGS);
        n = GEOB_MAX_BUILDINGS;
    }
    s_gb.b = (TD5_GeoBuilding *)calloc((size_t)(n > 0 ? n : 1),
                                       sizeof(TD5_GeoBuilding));
    if (!s_gb.b) { cJSON_Delete(root); return 0; }

    /* [ROUND 1012 D1] WALK the list, never cJSON_GetArrayItem(arr, i): that
     * call starts at the head every time, so the loop was O(n^2) -- harmless
     * at 2326 OSM footprints, 78 s of load at 90840 (MEASURED: R14 GENPERF
     * "generation took 78.4 s" against 0.3 s, with every listed zone under
     * 0.2 s). Same element order, so the load is unchanged. */
    e_next = arr->child;
    for (i = 0; i < n; i++) {
        const cJSON *e = e_next;
        TD5_GeoBuilding *b = &s_gb.b[s_gb.nb];
        e_next = e ? e->next : NULL;
        const char *hs;
        double h_m, mh_m, rh_m;
        int first = 0, rn;

        rn = geob_push_ring(e ? cJSON_GetObjectItem(e, "points") : NULL,
                            &first, &b->cx, &b->cz, &b->radius);
        if (rn == 0) continue;
        b->first = first;
        b->n     = rn;

        h_m  = geob_num_tag(e, "height_m", 0.0);
        mh_m = geob_num_tag(e, "min_height_m", 0.0);
        rh_m = geob_num_tag(e, "roof_height_m", 0.0);
        /* A footprint with no usable height is not a building we can extrude;
         * one storey is the floor rather than dropping it, since the footprint
         * itself is the fact worth keeping. */
        if (!(h_m > 0.0)) h_m = s_gb.storey_m;
        /* min_height is the BASE of the mass and height is its TOP, both
         * measured from the ground (OSM Simple 3D Buildings). A part whose
         * base is at or above its top is a tagging error; drop the base rather
         * than extrude a mass the wrong way up. */
        if (mh_m >= h_m) mh_m = 0.0;
        b->height      = h_m  * s_gb.units_per_m;
        b->min_height  = (mh_m > 0.0) ? mh_m * s_gb.units_per_m : 0.0;
        b->roof_height = (rh_m > 0.0) ? rh_m * s_gb.units_per_m : 0.0;
        b->area_m2    = geob_num(e, "area_m2", 0.0);
        b->id_hash    = geob_id_hash(geob_num(e, "id", (double)i));
        b->roof       = (unsigned char)geob_roof_of(geob_str(e, "roof_shape"));
        /* LANDMARK.
         *
         * ON (the default): geo_fetch's own `landmark_src` is believed first,
         * because it was decided against the WHOLE tag set including keys not
         * promoted to fields. Then the C tag rule, for a cache land_relabel
         * upgraded. Then the bare bool, which is all a tag_schema 1 cache has
         * and carries no reason to report.
         *
         * OFF (TD5RE_GEO_LM_TAGS=0): the PRE-2026-10-07 rule, RECONSTRUCTED
         * rather than approximated. The new cache's own `landmark` bool is no
         * use for the A/B -- it already contains the tag decision -- but the
         * old Python rule was `tourism or historic or (name and building in
         * <narrow set>)` and every one of those fields is in the record, so it
         * can be recomputed exactly and OR'd with the class rule the old
         * reader applied. That makes the knob a true A/B on one cache instead
         * of a third behaviour that resembles neither build. */
        {
            int src;
            if (tag_lm) {
                src = geob_lmsrc_of_string(geob_str(e, "landmark_src"));
                if (!src) src = geob_tag_landmark_src(e);
                if (!src && geob_true(e, "landmark"))
                    src = TD5_GEOB_LMSRC_FLAG;
            } else if (geob_legacy_landmark(e)) {
                src = TD5_GEOB_LMSRC_FLAG;
            } else {
                src = geob_class_is_landmark(geob_str(e, "class"))
                    ? TD5_GEOB_LMSRC_BUILDING : TD5_GEOB_LMSRC_NONE;
            }
            b->landmark = (unsigned char)(src != TD5_GEOB_LMSRC_NONE);
            b->lmsrc    = (unsigned char)src;
        }
        b->part       = (unsigned char)(geob_true(e, "part") ? 1 : 0);
        b->host_span  = -1;
        b->host_side  = 0;

        hs = geob_str(e, "height_src");
        if (hs && !strcmp(hs, "osm_height"))      b->hsrc = TD5_GEOB_HSRC_OSM_HEIGHT;
        else if (hs && !strcmp(hs, "osm_levels")) b->hsrc = TD5_GEOB_HSRC_OSM_LEVELS;
        else if (hs && !strncmp(hs, "overture_", 9))
                                                  b->hsrc = TD5_GEOB_HSRC_OVERTURE;
        else if (hs && !strcmp(hs, "openbuildings_raster"))
                                                  b->hsrc = TD5_GEOB_HSRC_RASTER;
        else                                      b->hsrc = TD5_GEOB_HSRC_ESTIMATED;
        {   /* [ROUND 1012 D1] absent on every pre-1012 record = OSM. */
            const char *fs = geob_str(e, "footprint_src");
            b->fsrc = (unsigned char)((fs && !strncmp(fs, "overture", 8))
                                      ? TD5_GEOB_FSRC_OVERTURE
                                      : TD5_GEOB_FSRC_OSM);
        }

        /* [ROUND 1009 item 9] STOREY COUNTS. `levels` is on every record in a
         * tag_schema-2 cache; `levels_est` is the estimator's own guess and is
         * the honest fallback for a footprint OSM never counted. Read through
         * geob_num_tag because Overpass hands every tag back as a STRING.
         * Clamped at 200: a levels tag of 1000 is a typo, and the emitter
         * divides the wall height by this to lay the facade page. */
        {
            double lv = geob_num_tag(e, "levels", 0.0);
            double rl = geob_num_tag(e, "roof_levels", 0.0);
            double ml = geob_num_tag(e, "min_level", 0.0);
            if (!(lv > 0.0)) lv = geob_num_tag(e, "levels_est", 0.0);
            if (lv > 200.0) lv = 200.0;
            if (rl > 200.0) rl = 200.0;
            if (ml > 200.0) ml = 200.0;
            b->levels      = (unsigned short)((lv > 0.0) ? (int)(lv + 0.5) : 0);
            b->roof_levels = (unsigned short)((rl > 0.0) ? (int)(rl + 0.5) : 0);
            b->min_level   = (unsigned short)((ml > 0.0) ? (int)(ml + 0.5) : 0);
        }

        s_gb.nb++;
    }
    cJSON_Delete(root);
    return s_gb.nb > 0;
}

static int geob_load_areas(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    const int wide = td5_env_flag_on("TD5RE_GEO_AREA_TAGS");
    int n, i;

    td5_geo_place_path(path, sizeof(path), slug, "AREAS.JSON");
    json = geob_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no readable AREAS.JSON; plazas stay "
                  "procedural", slug);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    arr = cJSON_GetObjectItem(root, "areas");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(root); return 0; }
    n = cJSON_GetArraySize(arr);
    if (n > GEOB_MAX_AREAS) n = GEOB_MAX_AREAS;
    s_gb.a = (TD5_GeoArea *)calloc((size_t)(n > 0 ? n : 1), sizeof(TD5_GeoArea));
    if (!s_gb.a) { cJSON_Delete(root); return 0; }

    for (i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        TD5_GeoArea *a = &s_gb.a[s_gb.na];
        int first = 0, rn;
        const char *nm;

        /* An unclosed way is a fence or a hedge line, not an area. */
        if (!geob_true(e, "closed")) continue;
        rn = geob_push_ring(e ? cJSON_GetObjectItem(e, "points") : NULL,
                            &first, &a->cx, &a->cz, &a->radius);
        if (rn == 0) continue;
        a->first = first;
        a->n     = rn;
        /* `natural` is new to this call (2026-10-07): the areas layer never
         * carried it, because geo_fetch only consulted `natural` for water. */
        a->kind  = (unsigned char)geob_area_kind_of(
                       geob_str(e, "leisure"), geob_str(e, "landuse"),
                       geob_str(e, "natural"), wide);
        a->id_hash = geob_id_hash(geob_num(e, "id", (double)i));
        nm = geob_str(e, "name");
        a->named = (unsigned char)((nm && nm[0]) ? 1 : 0);
        /* [ROUND 1009 item 4] The area's OWN barrier. "there's no such wall at
         * the end of this park" -- and OSM agrees: 414 of La Plata's 417 areas
         * carry no `barrier` at all. The field has always been in AREAS.JSON;
         * this layer simply never read it, so the plaza emitter's boundary
         * hedge was derived geometry with nothing behind it. */
        {
            const char *bv = geob_str(e, "barrier");
            a->barrier = TD5_GEOA_BARRIER_NONE;
            if (bv && bv[0]) {
                if (!strcmp(bv, "hedge"))      a->barrier = TD5_GEOA_BARRIER_HEDGE;
                else if (!strcmp(bv, "wall")
                         || !strcmp(bv, "retaining_wall")
                         || !strcmp(bv, "city_wall")) a->barrier = TD5_GEOA_BARRIER_WALL;
                else                           a->barrier = TD5_GEOA_BARRIER_FENCE;
            }
        }
        a->host_span = -1;
        a->host_side = 0;
        s_gb.na++;
    }
    cJSON_Delete(root);
    return s_gb.na > 0;
}

/* ------------------------------------------------------------- lifecycle --- */

/* Defined with the rest of the binding below; the loader is the only caller. */
/* ======================================================================== *
 * [ROUND 1013 F1] PLAZAS THAT ARE ONLY A RING, AND WHAT STANDS IN ANY PLAZA
 * ======================================================================== *
 *
 * "there's still buildings on the plazas."
 *
 * MEASURED on Mariano's route (MODELS.DAT + MESHTAG.BIN decode of the generated
 * level, then a top-down plot of the meshes over the map): Plaza Miguel de
 * Azcuenaga has NO leisure=park / place=square polygon -- it exists only as
 * ten junction=circular road ways around an open centre -- and 80 tall meshes
 * stood inside that ring: 47 `building` (the procedural frontage and back rows
 * facing the square), 29 `cross` (the walls and flanks of side streets) and 4
 * `city`. Every stand-down probe in the generator answers through
 * td5_geob_points_in_plaza, which knew only POLYGONS, so the frontage walled the
 * square in. The three square polygons that DO exist on the route (Plaza
 * Mariano Moreno, Plaza Maximo Paz, the start park) had no procedural mesh in
 * them already; this is the one the polygon test could not see.
 *
 * THE INTERIOR OF A RING IS ITS CONVEX HULL. The loop is the street around the
 * square, so the hull of the named group's points is bounded by the road
 * centreline and everything beyond it, across the street, is outside. (The disc
 * the set-piece test uses -- centroid + max distance -- is fine for a
 * conservative "no set piece here" but would swallow the frontage across the
 * street: the ring's corner is 99 m out and its side 70 m.)
 *
 * THE REAL FOOTPRINTS are the other half. Overture adds 88k footprints and some
 * are plaza features (kiosks, monuments, a bandstand), not buildings. A
 * footprint is VETOED when most of its outline stands inside a plaza (polygon or
 * ring) and it is neither a landmark nor a small OSM-tagged building (< 60 m2).
 * The veto only stops EMISSION: the stand-down probes still see the footprint,
 * so the procedural frontage that was standing down for it keeps standing down. */

#define GEOB_NO_PLAZA  (-2147483647 - 1)

static double geob_ring_area_m2(const TD5_GeoBuilding *b)
{
    double a;
    if (b->n < 3 || !(s_gb.units_per_m > 0.0)) return b->area_m2;
    a = fabs(td5_geob_ring_area(&s_gb.px[b->first], &s_gb.pz[b->first], b->n));
    return a / (s_gb.units_per_m * s_gb.units_per_m);
}

/* Andrew's monotone chain over `n` points; writes at most `cap` hull vertices.
 * Returns the hull size, 0 for a degenerate set, -1 when it would not fit.
 * `ix` is scratch (n ints). */
static int geob_hull(const double *x, const double *z, int n, int *ix,
                     double *hx, double *hz, int cap)
{
    int i, k = 0, lo;
    int *h;
    /* indices sorted by (x, z): insertion sort, n is a few hundred */
    for (i = 0; i < n; i++) {
        int j = i;
        while (j > 0 && (x[ix[j - 1]] > x[i] ||
               (x[ix[j - 1]] == x[i] && z[ix[j - 1]] > z[i]))) {
            ix[j] = ix[j - 1];
            j--;
        }
        ix[j] = i;
    }
#define GEOB_CROSS(o, a, b) \
    ((x[a] - x[o]) * (z[b] - z[o]) - (z[a] - z[o]) * (x[b] - x[o]))
    h = (int *)malloc((size_t)(2 * n + 2) * sizeof(int));
    if (!h) return -1;
    for (i = 0; i < n; i++) {
        while (k >= 2 && GEOB_CROSS(h[k - 2], h[k - 1], ix[i]) <= 0.0) k--;
        h[k++] = ix[i];
    }
    lo = k + 1;
    for (i = n - 2; i >= 0; i--) {
        while (k >= lo && GEOB_CROSS(h[k - 2], h[k - 1], ix[i]) <= 0.0) k--;
        h[k++] = ix[i];
    }
    k--;                                   /* last == first */
#undef GEOB_CROSS
    if (k < 3) { free(h); return 0; }
    if (k > cap) { free(h); return -1; }
    for (i = 0; i < k; i++) { hx[i] = x[h[i]]; hz[i] = z[h[i]]; }
    free(h);
    return k;
}

/* Collect every NAMED junction=circular/roundabout group as one hull. Same
 * selection as td5_tg_city.c's tg_geo_rings_collect (the set-piece veto), so
 * the two never disagree on which squares exist. */
static void geob_collect_plaza_rings(void)
{
    int n, i, q, k;
    int seen[GEOB_PRING_MAX], n_seen = 0;

    s_gb.n_pring = 0;
    s_gb.pring_on = td5_env_flag_on("TD5RE_GEO_PLAZA_RING");
    if (!s_gb.pring_on) return;
    td5_geo_roads_sync(td5_geo_place_slug());
    n = td5_geo_roads_count();
    for (i = 0; i < n && s_gb.n_pring < GEOB_PRING_MAX; i++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(i);
        double *x, *z, cx = 0.0, cz = 0.0, rad = 0.0;
        int *ix, np = 0, nh;
        if (!r || !r->roundabout || r->count < 3 || r->name_id < 0) continue;
        for (q = 0; q < n_seen; q++) if (seen[q] == r->name_id) break;
        if (q < n_seen) continue;
        if (n_seen < GEOB_PRING_MAX) seen[n_seen++] = r->name_id;
        for (q = 0; q < n; q++) {
            const TD5_GeoRoad *w = td5_geo_roads_get(q);
            if (w && w->roundabout && w->name_id == r->name_id) np += w->count;
        }
        if (np < 3) continue;
        x  = (double *)malloc((size_t)np * sizeof(double));
        z  = (double *)malloc((size_t)np * sizeof(double));
        ix = (int *)malloc((size_t)np * sizeof(int));
        if (!x || !z || !ix) { free(x); free(z); free(ix); return; }
        np = 0;
        for (q = 0; q < n; q++) {
            const TD5_GeoRoad *w = td5_geo_roads_get(q);
            if (!w || !w->roundabout || w->name_id != r->name_id) continue;
            for (k = 0; k < w->count; k++) {
                double px, pz;
                if (!td5_geo_roads_point(w, k, &px, &pz)) continue;
                x[np] = px; z[np] = pz; cx += px; cz += pz; np++;
            }
        }
        cx /= (double)np; cz /= (double)np;
        for (k = 0; k < np; k++) {
            const double d = hypot(x[k] - cx, z[k] - cz);
            if (d > rad) rad = d;
        }
        nh = geob_hull(x, z, np, ix,
                       s_gb.pring[s_gb.n_pring].hx, s_gb.pring[s_gb.n_pring].hz,
                       GEOB_PRING_PTS);
        if (nh >= 3 && rad > 0.0) {
            s_gb.pring[s_gb.n_pring].cx = cx;
            s_gb.pring[s_gb.n_pring].cz = cz;
            s_gb.pring[s_gb.n_pring].r  = rad;
            s_gb.pring[s_gb.n_pring].n  = nh;
            snprintf(s_gb.pring[s_gb.n_pring].name,
                     sizeof s_gb.pring[0].name, "ring #%d", r->name_id);
            s_gb.n_pring++;
        } else {
            TD5_LOG_W(LOG_TAG, "geob: plaza ring group %d skipped (hull %d "
                      "vertices, cap %d)", r->name_id, nh, GEOB_PRING_PTS);
        }
        free(x); free(z); free(ix);
    }
    TD5_LOG_I(LOG_TAG, "geob: %d named plaza ring(s) kept as hulls (their "
              "interior is a plaza: no procedural frontage, no footprint "
              "mass bigger than a kiosk)", s_gb.n_pring);
}

/* Index of the ring whose hull holds (x,z), or -1. */
static int geob_plaza_ring_at(double x, double z)
{
    int i;
    for (i = 0; i < s_gb.n_pring; i++) {
        if (hypot(x - s_gb.pring[i].cx, z - s_gb.pring[i].cz)
            > s_gb.pring[i].r + 1.0) continue;
        if (td5_geob_point_in_ring(s_gb.pring[i].hx, s_gb.pring[i].hz,
                                   s_gb.pring[i].n, x, z)) return i;
    }
    return -1;
}

int td5_geob_in_plaza_ring(double x, double z)
{
    if (!s_gb.loaded || !s_gb.pring_on) return 0;
    return geob_plaza_ring_at(x, z) >= 0;
}

int td5_geob_plaza_ring_count(void) { return s_gb.loaded ? s_gb.n_pring : 0; }

int td5_geob_plaza_ring_get(int i, double *cx, double *cz, double *r)
{
    if (!s_gb.loaded || i < 0 || i >= s_gb.n_pring) return 0;
    if (cx) *cx = s_gb.pring[i].cx;
    if (cz) *cz = s_gb.pring[i].cz;
    if (r)  *r  = s_gb.pring[i].r;
    return 1;
}

void td5_geob_plaza_veto_stats(int *vetoed, int *kept_small, int *kept_landmark)
{
    if (vetoed)        *vetoed        = s_gb.veto_n;
    if (kept_small)    *kept_small    = s_gb.veto_small;
    if (kept_landmark) *kept_landmark = s_gb.veto_landmark;
}

/* Which plaza holds the point? >= 0 an AREAS.JSON index, -(1 + r) for ring r,
 * GEOB_NO_PLAZA for none. Polygon plazas are the ones td5_geob_area_is_plaza
 * names. */
static int geob_plaza_of_point(double x, double z)
{
    int i;
    for (i = 0; i < s_gb.na; i++) {
        const TD5_GeoArea *a = &s_gb.a[i];
        if (!td5_geob_area_is_plaza(a)) continue;
        if (hypot(x - a->cx, z - a->cz) > a->radius) continue;
        if (td5_geob_point_in_ring(&s_gb.px[a->first], &s_gb.pz[a->first],
                                   a->n, x, z)) return i;
    }
    i = s_gb.pring_on ? geob_plaza_ring_at(x, z) : -1;
    return i >= 0 ? -(1 + i) : GEOB_NO_PLAZA;
}

/* Veto the bound footprints that stand in a plaza. Runs once per place load,
 * after the bind, so only the footprints the emitter could ever reach are
 * tested (~1.9k of 90k on La Plata). */
static void geob_veto_plaza(void)
{
    const int nslot = s_gb.na + GEOB_PRING_MAX;
    int *cnt, *keep, i, k, total = 0;

    s_gb.veto_n = s_gb.veto_small = s_gb.veto_landmark = 0;
    if (!td5_env_flag_on("TD5RE_GEO_PLAZA_BLD")) return;
    cnt  = (int *)calloc((size_t)nslot, sizeof(int));
    keep = (int *)calloc((size_t)nslot, sizeof(int));
    if (!cnt || !keep) { free(cnt); free(keep); return; }

    for (i = 0; i < s_gb.nb; i++) {
        TD5_GeoBuilding *b = &s_gb.b[i];
        int in = 0, np = 0, which, slot;
        if (b->host_span < 0 || b->n < 3) continue;
        for (k = -1; k < b->n; k++) {
            const double px = (k < 0) ? b->cx : s_gb.px[b->first + k];
            const double pz = (k < 0) ? b->cz : s_gb.pz[b->first + k];
            np++;
            if (geob_plaza_of_point(px, pz) != GEOB_NO_PLAZA) in++;
        }
        if (in * 2 <= np) continue;                       /* mostly outside */
        which = geob_plaza_of_point(b->cx, b->cz);
        for (k = 0; k < b->n && which == GEOB_NO_PLAZA; k++)
            which = geob_plaza_of_point(s_gb.px[b->first + k],
                                        s_gb.pz[b->first + k]);
        slot = which >= 0 ? which : s_gb.na + (-which - 1);
        if (which == GEOB_NO_PLAZA || slot < 0 || slot >= nslot) continue;
        total++;
        if (b->landmark) { s_gb.veto_landmark++; keep[slot]++; continue; }
        if (b->fsrc == TD5_GEOB_FSRC_OSM &&
            geob_ring_area_m2(b) < GEOB_PLAZA_KEEP_M2) {
            s_gb.veto_small++; keep[slot]++; continue;
        }
        b->plaza_veto = 1;
        s_gb.veto_n++;
        cnt[slot]++;
    }
    TD5_LOG_I(LOG_TAG, "geob: %d bound footprint(s) stand in a plaza: %d "
              "dropped (not a landmark, not a small OSM building), %d kept as "
              "landmarks, %d kept as small OSM buildings (< %.0f m2)",
              total, s_gb.veto_n, s_gb.veto_landmark, s_gb.veto_small,
              GEOB_PLAZA_KEEP_M2);
    for (i = 0; i < nslot; i++) {
        if (!cnt[i] && !keep[i]) continue;
        if (i < s_gb.na)
            TD5_LOG_I(LOG_TAG, "geob:   %s %d (kind %d, centre %.0f,%.0f "
                      "r %.0f m): %d dropped, %d kept",
                      s_gb.a[i].ring ? "ring plaza area" : "plaza area",
                      i, (int)s_gb.a[i].kind,
                      s_gb.a[i].cx, s_gb.a[i].cz,
                      s_gb.a[i].radius / s_gb.units_per_m, cnt[i], keep[i]);
        else
            TD5_LOG_I(LOG_TAG, "geob:   plaza %s (centre %.0f,%.0f r %.0f m): "
                      "%d dropped, %d kept", s_gb.pring[i - s_gb.na].name,
                      s_gb.pring[i - s_gb.na].cx, s_gb.pring[i - s_gb.na].cz,
                      s_gb.pring[i - s_gb.na].r / s_gb.units_per_m,
                      cnt[i], keep[i]);
    }
    free(cnt); free(keep);
}

/* ---- [ROUND 1013 F1 follow-up] A RING PLAZA IS FILLED LIKE ANY OTHER ----------
 *
 * "The ring plaza interior is now bare paved ground, which will read as a car
 * park, not a plaza."
 *
 * The plaza emitter (td5_tg_streets.c tg_geo_emit_plaza) lays a lawn, paths and
 * trees for every bound AREAS.JSON polygon whose kind td5_geob_area_is_plaza
 * names. A square mapped only as a ring has no polygon, so it got nothing. Rather
 * than write a second emitter, each ring group becomes ONE synthesised
 * TD5_GeoArea (kind PARK, ring=1) and goes through the same bind and the same
 * emitter.
 *
 * THE OUTLINE IS THE HULL, INSET. The hull is bounded by the ring road's
 * CENTRELINE, so a lawn over the hull itself would run onto the carriageway and
 * its pavement. It is pulled in by GEOB_PRING_INSET_M = 9 m: measured on Mariano's
 * route the ring edge is a 2-lane carriageway (3 m half width) with about 4.5 m
 * of pavement and railing, so 9 m leaves 1.5 m of ground between railing and
 * lawn. (11 m, the first cut, left a grey rim the width of a second pavement.)
 * The plaza emitter then pushes any vertex still inside the carriageway +
 * pavement clearance outward again (tg_geop_project), so a wider ring road costs
 * the lawn some edge, never a lane. The hull is convex, so the inset is the intersection of
 * its edges moved inward: each vertex goes along the bisector of its two edge
 * normals by d / cos(half the turn), clamped on a sharp corner.
 *
 * A RING THAT A REAL POLYGON ALREADY COVERS IS SKIPPED (the hull centre inside a
 * plaza polygon at least 0.6 as big), so a square mapped BOTH ways is not laid
 * twice. */
#define GEOB_PRING_INSET_M  9.0

/* Move a convex CCW ring inward by `d` world units. 0 when it collapses. */
static int geob_inset_convex(const double *x, const double *z, int n, double d,
                             double *ox, double *oz)
{
    int i;
    for (i = 0; i < n; i++) {
        const int a = (i + n - 1) % n, b = (i + 1) % n;
        double e1x = x[i] - x[a], e1z = z[i] - z[a];
        double e2x = x[b] - x[i], e2z = z[b] - z[i];
        const double l1 = hypot(e1x, e1z), l2 = hypot(e2x, e2z);
        double n1x, n1z, n2x, n2z, dot, k;
        if (!(l1 > 1e-9) || !(l2 > 1e-9)) return 0;
        /* inward normal of a CCW edge is its LEFT normal (-dz, dx) */
        n1x = -e1z / l1; n1z = e1x / l1;
        n2x = -e2z / l2; n2z = e2x / l2;
        dot = n1x * n2x + n1z * n2z;
        k = 1.0 + dot;
        if (k < 0.2) k = 0.2;                  /* sharp corner: bound the miter */
        ox[i] = x[i] + (n1x + n2x) * d / k;
        oz[i] = z[i] + (n1z + n2z) * d / k;
    }
    /* Collapsed or inverted? A convex ring moved inward keeps its winding. */
    if (td5_geob_ring_area(ox, oz, n) <= 0.0) return 0;
    return n;
}

/* Drop the vertex whose removal changes the outline least until `cap` remain. */
static int geob_decimate_ring(double *x, double *z, int n, int cap)
{
    while (n > cap) {
        int i, best = 0;
        double ba = 1e300;
        for (i = 0; i < n; i++) {
            const int a = (i + n - 1) % n, b = (i + 1) % n;
            const double ar = fabs((x[i] - x[a]) * (z[b] - z[a])
                                 - (z[i] - z[a]) * (x[b] - x[a]));
            if (ar < ba) { ba = ar; best = i; }
        }
        for (i = best; i + 1 < n; i++) { x[i] = x[i + 1]; z[i] = z[i + 1]; }
        n--;
    }
    return n;
}

/* A plaza POLYGON that already covers ring `r`? Run BEFORE the ring areas are
 * appended, so only AREAS.JSON polygons are asked. */
static int geob_ring_covered_by_polygon(int r)
{
    int i;
    for (i = 0; i < s_gb.na; i++) {
        const TD5_GeoArea *a = &s_gb.a[i];
        if (a->ring || !td5_geob_area_is_plaza(a)) continue;
        if (a->radius < 0.6 * s_gb.pring[r].r) continue;
        if (td5_geob_point_in_ring(&s_gb.px[a->first], &s_gb.pz[a->first],
                                   a->n, s_gb.pring[r].cx, s_gb.pring[r].cz))
            return 1;
    }
    return 0;
}

/* Append one synthesised area per ring group. Runs after the ring hulls are
 * known and BEFORE geob_bind_all, so the new records are bound to spans exactly
 * like the polygons. TD5RE_GEO_PLAZA_RING_FILL=0 pins the 1013-F1 first cut
 * (ring interior left bare). */
static void geob_add_ring_areas(void)
{
    double inset;
    int r, added = 0, covered = 0, collapsed = 0;

    if (!s_gb.pring_on || s_gb.n_pring < 1) return;
    if (!td5_env_flag_on("TD5RE_GEO_PLAZA_RING_FILL")) return;
    inset = GEOB_PRING_INSET_M * s_gb.units_per_m;
    {
        TD5_GeoArea *grown = (TD5_GeoArea *)realloc(
            s_gb.a, (size_t)(s_gb.na + s_gb.n_pring) * sizeof(TD5_GeoArea));
        if (!grown) return;
        s_gb.a = grown;
    }
    for (r = 0; r < s_gb.n_pring; r++) {
        double x[GEOB_PRING_PTS], z[GEOB_PRING_PTS];
        double ix[GEOB_PRING_PTS], iz[GEOB_PRING_PTS];
        TD5_GeoArea *a;
        int n = s_gb.pring[r].n, k;
        double cx = 0.0, cz = 0.0, rad = 0.0;

        if (geob_ring_covered_by_polygon(r)) { covered++; continue; }
        for (k = 0; k < n; k++) { x[k] = s_gb.pring[r].hx[k]; z[k] = s_gb.pring[r].hz[k]; }
        if (td5_geob_ring_area(x, z, n) < 0.0) {            /* force CCW */
            for (k = 0; k < n / 2; k++) {
                double t = x[k]; x[k] = x[n - 1 - k]; x[n - 1 - k] = t;
                t = z[k]; z[k] = z[n - 1 - k]; z[n - 1 - k] = t;
            }
        }
        if (!geob_inset_convex(x, z, n, inset, ix, iz)) { collapsed++; continue; }
        n = geob_decimate_ring(ix, iz, n, TD5_GEOB_RING_MAX);
        if (n < 3 || !geob_pool_reserve(s_gb.npt + n)) { collapsed++; continue; }
        for (k = 0; k < n; k++) { cx += ix[k]; cz += iz[k]; }
        cx /= (double)n; cz /= (double)n;
        for (k = 0; k < n; k++) {
            const double d = hypot(ix[k] - cx, iz[k] - cz);
            if (d > rad) rad = d;
        }
        a = &s_gb.a[s_gb.na];
        memset(a, 0, sizeof *a);
        a->first = s_gb.npt;
        a->n = n;
        for (k = 0; k < n; k++) {
            s_gb.px[s_gb.npt] = ix[k];
            s_gb.pz[s_gb.npt] = iz[k];
            s_gb.npt++;
        }
        a->cx = cx; a->cz = cz; a->radius = rad;
        a->kind = TD5_GEOA_KIND_PARK;
        a->named = 1;
        a->barrier = TD5_GEOA_BARRIER_NONE;
        a->ring = 1;
        a->id_hash = geob_id_hash((double)(1000000 + r));
        a->host_span = -1;
        a->host_side = 0;
        s_gb.na++;
        added++;
    }
    TD5_LOG_I(LOG_TAG, "geob: %d ring plaza(s) laid as areas (hull inset "
              "%.0f m clear of the ring road): %d skipped because a mapped "
              "polygon already covers them, %d too small to inset", added,
              GEOB_PRING_INSET_M, covered, collapsed);
}

static void geob_bind_all(void);

void td5_geob_unload(void)
{
    free(s_gb.b);
    free(s_gb.a);
    free(s_gb.px);
    free(s_gb.pz);
    free(s_gb.b_head);
    free(s_gb.a_head);
    free(s_gb.b_next);
    free(s_gb.a_next);
    memset(&s_gb, 0, sizeof(s_gb));
}

int td5_geob_sync(void)
{
    const char *slug = td5_geo_place_slug();

    if (!slug || !slug[0]) {
        if (s_gb.loaded) td5_geob_unload();
        return 0;
    }
    if (s_gb.loaded && !strcmp(slug, s_gb.slug)) return 1;

    td5_geob_unload();
    snprintf(s_gb.slug, sizeof(s_gb.slug), "%s", slug);
    s_gb.storey_m = 3.0;
    if (!geob_pool_reserve(GEOB_POOL_INITIAL)) {
        TD5_LOG_E(LOG_TAG, "geob: out of memory for the ring pool");
        td5_geob_unload();
        return 0;
    }
    geob_load_place_scale(slug);
    geob_load_buildings(slug);
    geob_load_areas(slug);

    if (s_gb.nb <= 0 && s_gb.na <= 0) {
        TD5_LOG_W(LOG_TAG, "geob: %s has no usable polygons; everything stays "
                  "procedural", slug);
        td5_geob_unload();
        return 0;
    }
    s_gb.loaded = 1;
    geob_collect_plaza_rings();
    geob_add_ring_areas();
    geob_bind_all();
    geob_veto_plaza();
    {
        int meas = 0, est = 0, lm = 0, roof = 0, plaza = 0, i;
        for (i = 0; i < s_gb.nb; i++) {
            if (s_gb.b[i].hsrc == TD5_GEOB_HSRC_ESTIMATED) est++; else meas++;
            if (s_gb.b[i].landmark) lm++;
            if (s_gb.b[i].roof != TD5_GEOB_ROOF_NONE) roof++;
        }
        for (i = 0; i < s_gb.na; i++)
            if (s_gb.a[i].kind == TD5_GEOA_KIND_PARK
                || s_gb.a[i].kind == TD5_GEOA_KIND_PLAY) plaza++;
        TD5_LOG_I(LOG_TAG, "geob: %s loaded %d building(s) (%d measured, %d "
                  "estimated, %d landmark, %d with roof:shape) and %d area(s) "
                  "(%d plaza/park); %d ring point(s), %.1f units/m, storey "
                  "%.1f m; decimated %d polygon(s) / %d point(s)",
                  slug, s_gb.nb, meas, est, lm, roof, s_gb.na, plaza,
                  s_gb.npt, s_gb.units_per_m, s_gb.storey_m,
                  s_gb.dec_polys, s_gb.dec_points);
        {   /* WHICH TAG decided each landmark. "22 landmarks" is as
             * informative as "22 hostels" -- and the pre-2026-10-07 blanket
             * `tourism or historic` really had promoted one. */
            int src[TD5_GEOB_LMSRC_COUNT];
            char line[256];
            int k, off = 0;
            td5_geob_landmark_sources(src, TD5_GEOB_LMSRC_COUNT);
            for (k = 1; k < TD5_GEOB_LMSRC_COUNT; k++) {
                if (!src[k]) continue;
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                "%s%s=%d", off ? ", " : "",
                                td5_geob_lmsrc_name(k), src[k]);
                if (off < 0 || (size_t)off >= sizeof(line)) break;
            }
            TD5_LOG_I(LOG_TAG, "geob: landmarks by deciding tag: %s",
                      off > 0 ? line : "(none)");
        }
        {   /* [ROUND 1012 D1] whose footprint, whose height. */
            int hs[TD5_GEOB_HSRC_COUNT], fs[2];
            td5_geob_source_census(hs, fs);
            TD5_LOG_I(LOG_TAG, "geob: footprints %d OSM / %d Overture; heights "
                      "%d OSM height / %d OSM levels / %d Overture / %d Open "
                      "Buildings raster / %d estimated",
                      fs[TD5_GEOB_FSRC_OSM], fs[TD5_GEOB_FSRC_OVERTURE],
                      hs[TD5_GEOB_HSRC_OSM_HEIGHT], hs[TD5_GEOB_HSRC_OSM_LEVELS],
                      hs[TD5_GEOB_HSRC_OVERTURE], hs[TD5_GEOB_HSRC_RASTER],
                      hs[TD5_GEOB_HSRC_ESTIMATED]);
        }
        TD5_LOG_I(LOG_TAG, "geob: bound to %d route span(s): %d building(s) "
                  "on the route / %d past %.0f units, %d area(s) on the route "
                  "/ %d past %.0f units",
                  s_gb.bound_spans, s_gb.b_bound, s_gb.b_far,
                  TD5_GEOB_BIND_MAX_B, s_gb.a_bound, s_gb.a_far,
                  TD5_GEOB_BIND_MAX_A);
    }
    return 1;
}

int td5_geob_loaded(void) { return s_gb.loaded; }

/* ---------------------------------------------------------------- access --- */

int td5_geob_building_count(void) { return s_gb.loaded ? s_gb.nb : 0; }
int td5_geob_area_count(void)     { return s_gb.loaded ? s_gb.na : 0; }

const TD5_GeoBuilding *td5_geob_building(int i)
{
    if (!s_gb.loaded || i < 0 || i >= s_gb.nb) return NULL;
    return &s_gb.b[i];
}

const TD5_GeoArea *td5_geob_area(int i)
{
    if (!s_gb.loaded || i < 0 || i >= s_gb.na) return NULL;
    return &s_gb.a[i];
}

void td5_geob_ring(int first, int k, double *x, double *z)
{
    const int i = first + k;
    if (!s_gb.loaded || first < 0 || k < 0 || i >= s_gb.npt) {
        if (x) *x = 0.0;
        if (z) *z = 0.0;
        return;
    }
    if (x) *x = s_gb.px[i];
    if (z) *z = s_gb.pz[i];
}

/* ------------------------------------------------------------- host bind --- */

/* Nearest route node to a ring, plus the side and lateral distance in the
 * generator's own convention: lateral = (dx*tz - dz*tx), POSITIVE = LEFT of
 * travel (see the CARRIAGEWAY QUERY block in td5_trackgen_internal.h), and the
 * outward unit for side sg is (tz, -tx) * sg.
 *
 * Chosen on the nearest RING VERTEX, not the centroid: a 100 m block whose
 * centroid is 60 m off the road still has a near wall on the street, and that
 * wall is the thing the driver sees. The lateral that comes back is the
 * CENTROID's, because that is where the building's base sits. */
/* [ROUND 1012 D1] ROUTE-NODE GRID for the bind. The plain search is
 * O(ring vertices x route nodes): 0.5 M vertices x 875 nodes = 460 M distance
 * tests on Overture-era La Plata. Nodes are bucketed in square cells of
 * GEOB_GRID_CELL world units (>= both bind radii) and a vertex only tests the
 * 3x3 cells around it. The answer is IDENTICAL to the full scan, ties
 * included: a building only binds when its best vertex-to-node distance is
 * <= max_near <= one cell, and every node that close sits in the 3x3 block;
 * candidates are visited in ascending node index (the per-cell lists are
 * built ascending and the 9 cells are merged by index), so the first minimum
 * wins exactly as before. A vertex whose block holds no node within max_near
 * can only lose to a vertex that has one, or be rejected, exactly as the full
 * scan would. */
#define GEOB_GRID_CELL 45000.0
static struct {
    int     n, nx, nz;
    double  x0, z0;
    double *rx, *rz;          /* node positions, index order             */
    int    *cell_head;        /* nx*nz, first (lowest) node in the cell   */
    int    *cell_tail;
    int    *next;             /* n, next node in the same cell, ascending */
} s_bg;

static void geob_grid_free(void)
{
    free(s_bg.rx); free(s_bg.rz);
    free(s_bg.cell_head); free(s_bg.cell_tail); free(s_bg.next);
    memset(&s_bg, 0, sizeof(s_bg));
}

static int geob_grid_build(void)
{
    const int nr = td5_geo_route_count();
    double x1, z1;
    int i, n = 0;
    geob_grid_free();
    if (nr < 2) return 0;
    s_bg.rx = (double *)malloc((size_t)nr * sizeof(double));
    s_bg.rz = (double *)malloc((size_t)nr * sizeof(double));
    s_bg.next = (int *)malloc((size_t)nr * sizeof(int));
    if (!s_bg.rx || !s_bg.rz || !s_bg.next) { geob_grid_free(); return 0; }
    for (i = 0; i < nr; i++) {
        if (!td5_geo_route_node(i, &s_bg.rx[i], &s_bg.rz[i], NULL)) break;
        n++;
    }
    /* The full scan stops at the first node the route cannot return; a grid
     * over fewer nodes than td5_geo_route_count() would still match it, but
     * keep the fallback honest and simple. */
    if (n != nr) { geob_grid_free(); return 0; }
    s_bg.n = n;
    s_bg.x0 = x1 = s_bg.rx[0];
    s_bg.z0 = z1 = s_bg.rz[0];
    for (i = 1; i < n; i++) {
        if (s_bg.rx[i] < s_bg.x0) s_bg.x0 = s_bg.rx[i];
        if (s_bg.rz[i] < s_bg.z0) s_bg.z0 = s_bg.rz[i];
        if (s_bg.rx[i] > x1) x1 = s_bg.rx[i];
        if (s_bg.rz[i] > z1) z1 = s_bg.rz[i];
    }
    s_bg.nx = (int)((x1 - s_bg.x0) / GEOB_GRID_CELL) + 1;
    s_bg.nz = (int)((z1 - s_bg.z0) / GEOB_GRID_CELL) + 1;
    s_bg.cell_head = (int *)malloc((size_t)s_bg.nx * (size_t)s_bg.nz
                                   * sizeof(int));
    s_bg.cell_tail = (int *)malloc((size_t)s_bg.nx * (size_t)s_bg.nz
                                   * sizeof(int));
    if (!s_bg.cell_head || !s_bg.cell_tail) { geob_grid_free(); return 0; }
    for (i = 0; i < s_bg.nx * s_bg.nz; i++)
        s_bg.cell_head[i] = s_bg.cell_tail[i] = -1;
    for (i = 0; i < n; i++) {           /* ascending: append at the tail */
        const int gx = (int)((s_bg.rx[i] - s_bg.x0) / GEOB_GRID_CELL);
        const int gz = (int)((s_bg.rz[i] - s_bg.z0) / GEOB_GRID_CELL);
        const int c = gz * s_bg.nx + gx;
        s_bg.next[i] = -1;
        if (s_bg.cell_tail[c] < 0) s_bg.cell_head[c] = i;
        else                       s_bg.next[s_bg.cell_tail[c]] = i;
        s_bg.cell_tail[c] = i;
    }
    return 1;
}

static int geob_bind_ring(int first, int n, double cx, double cz,
                          double max_near, int *out_span, int *out_side,
                          double *out_lat)
{
    const int nr = td5_geo_route_count();
    double best_d2 = -1.0;
    int best = -1, k, i;

    if (nr < 2) return 0;
    if (s_bg.n == nr && max_near <= GEOB_GRID_CELL) {
        for (k = 0; k < n; k++) {
            const double vx = s_gb.px[first + k], vz = s_gb.pz[first + k];
            const int gx = (int)floor((vx - s_bg.x0) / GEOB_GRID_CELL);
            const int gz = (int)floor((vz - s_bg.z0) / GEOB_GRID_CELL);
            int cur[9], m = 0, a, b;
            for (b = gz - 1; b <= gz + 1; b++) {
                if (b < 0 || b >= s_bg.nz) continue;
                for (a = gx - 1; a <= gx + 1; a++) {
                    if (a < 0 || a >= s_bg.nx) continue;
                    cur[m++] = s_bg.cell_head[b * s_bg.nx + a];
                }
            }
            for (;;) {                  /* k-way merge by ascending index */
                int pick = -1, j;
                double dx, dz, d2;
                for (j = 0; j < m; j++)
                    if (cur[j] >= 0 && (pick < 0 || cur[j] < cur[pick]))
                        pick = j;
                if (pick < 0) break;
                i = cur[pick];
                cur[pick] = s_bg.next[i];
                dx = vx - s_bg.rx[i]; dz = vz - s_bg.rz[i];
                d2 = dx * dx + dz * dz;
                if (best_d2 < 0.0 || d2 < best_d2) { best_d2 = d2; best = i; }
            }
        }
    } else {
        for (k = 0; k < n; k++) {
            const double vx = s_gb.px[first + k], vz = s_gb.pz[first + k];
            for (i = 0; i < nr; i++) {
                double rx, rz, dx, dz, d2;
                if (!td5_geo_route_node(i, &rx, &rz, NULL)) break;
                dx = vx - rx; dz = vz - rz;
                d2 = dx * dx + dz * dz;
                if (best_d2 < 0.0 || d2 < best_d2) { best_d2 = d2; best = i; }
            }
        }
    }
    if (best < 0 || sqrt(best_d2) > max_near) return 0;

    {   /* Tangent from the forward chord, which is what the engine integrates
         * (x += sin(h)*span; z += cos(h)*span), so it matches the node's own
         * heading to within the resampler's tolerance. */
        double ax, az, bx, bz, tx, tz, len, lat;
        const int j = (best + 1 < nr) ? best + 1 : best;
        const int p = (best > 0) ? best - 1 : best;
        if (!td5_geo_route_node(p, &ax, &az, NULL)
            || !td5_geo_route_node(j, &bx, &bz, NULL)) return 0;
        tx = bx - ax; tz = bz - az;
        len = hypot(tx, tz);
        if (!(len > 0.0)) return 0;
        tx /= len; tz /= len;
        if (!td5_geo_route_node(best, &ax, &az, NULL)) return 0;
        lat = (cx - ax) * tz - (cz - az) * tx;
        *out_span = best;
        *out_side = (lat >= 0.0) ? 1 : -1;
        *out_lat  = fabs(lat);
    }
    return 1;
}

/* Bind every polygon and chain it into its span bucket. Runs once per place
 * load, single-threaded, and allocates nothing afterwards -- see the header on
 * why a lazy bind from inside the scenery loop would be a race. */
static void geob_bind_all(void)
{
    const int nr = td5_geo_route_count();
    int i;

    s_gb.bound_spans = 0;
    s_gb.b_bound = s_gb.b_far = s_gb.a_bound = s_gb.a_far = 0;
    if (nr < 2) {
        TD5_LOG_W(LOG_TAG, "geob: no conditioned route loaded; real buildings "
                  "and plazas have no span to stand beside");
        return;
    }

    free(s_gb.b_head); free(s_gb.a_head);
    free(s_gb.b_next); free(s_gb.a_next);
    s_gb.b_head = (int *)malloc((size_t)nr * sizeof(int));
    s_gb.a_head = (int *)malloc((size_t)nr * sizeof(int));
    s_gb.b_next = (int *)malloc((size_t)(s_gb.nb > 0 ? s_gb.nb : 1) * sizeof(int));
    s_gb.a_next = (int *)malloc((size_t)(s_gb.na > 0 ? s_gb.na : 1) * sizeof(int));
    if (!s_gb.b_head || !s_gb.a_head || !s_gb.b_next || !s_gb.a_next) {
        free(s_gb.b_head); free(s_gb.a_head);
        free(s_gb.b_next); free(s_gb.a_next);
        s_gb.b_head = s_gb.a_head = s_gb.b_next = s_gb.a_next = NULL;
        TD5_LOG_E(LOG_TAG, "geob: out of memory for the per-span chains");
        return;
    }
    for (i = 0; i < nr; i++) { s_gb.b_head[i] = -1; s_gb.a_head[i] = -1; }
    geob_grid_build();

    for (i = 0; i < s_gb.nb; i++) {
        TD5_GeoBuilding *b = &s_gb.b[i];
        int sp = -1, sd = 0;
        double lat = 0.0;
        b->host_span = -1; b->host_side = 0; b->host_lat = 0.0;
        if (geob_bind_ring(b->first, b->n, b->cx, b->cz,
                           TD5_GEOB_BIND_MAX_B, &sp, &sd, &lat)) {
            b->host_span = sp; b->host_side = sd; b->host_lat = lat;
            s_gb.b_bound++;
        } else {
            s_gb.b_far++;
        }
    }
    for (i = 0; i < s_gb.na; i++) {
        TD5_GeoArea *a = &s_gb.a[i];
        int sp = -1, sd = 0;
        double lat = 0.0;
        a->host_span = -1; a->host_side = 0; a->host_lat = 0.0;
        if (geob_bind_ring(a->first, a->n, a->cx, a->cz,
                           TD5_GEOB_BIND_MAX_A, &sp, &sd, &lat)) {
            a->host_span = sp; a->host_side = sd; a->host_lat = lat;
            s_gb.a_bound++;
        } else {
            s_gb.a_far++;
        }
    }

    geob_grid_free();         /* bind-time only; nothing reads it later */

    /* Walk backwards so each chain ends up in ASCENDING index order, which
     * makes the emitted mesh order a pure function of the cache file. */
    for (i = s_gb.nb - 1; i >= 0; i--) {
        const int sp = s_gb.b[i].host_span;
        if (sp < 0 || sp >= nr) { s_gb.b_next[i] = -1; continue; }
        s_gb.b_next[i] = s_gb.b_head[sp];
        s_gb.b_head[sp] = i;
    }
    for (i = s_gb.na - 1; i >= 0; i--) {
        const int sp = s_gb.a[i].host_span;
        if (sp < 0 || sp >= nr) { s_gb.a_next[i] = -1; continue; }
        s_gb.a_next[i] = s_gb.a_head[sp];
        s_gb.a_head[sp] = i;
    }
    s_gb.bound_spans = nr;
}

int td5_geob_bound_spans(void) { return s_gb.loaded ? s_gb.bound_spans : 0; }

void td5_geob_bind_stats(int *buildings_bound, int *buildings_far,
                         int *areas_bound, int *areas_far)
{
    if (buildings_bound) *buildings_bound = s_gb.b_bound;
    if (buildings_far)   *buildings_far   = s_gb.b_far;
    if (areas_bound)     *areas_bound     = s_gb.a_bound;
    if (areas_far)       *areas_far       = s_gb.a_far;
}

int td5_geob_span_building(int span)
{
    if (!s_gb.loaded || !s_gb.b_head || span < 0 || span >= s_gb.bound_spans)
        return -1;
    return s_gb.b_head[span];
}

int td5_geob_next_building(int i)
{
    if (!s_gb.loaded || !s_gb.b_next || i < 0 || i >= s_gb.nb) return -1;
    return s_gb.b_next[i];
}

int td5_geob_span_area(int span)
{
    if (!s_gb.loaded || !s_gb.a_head || span < 0 || span >= s_gb.bound_spans)
        return -1;
    return s_gb.a_head[span];
}

int td5_geob_next_area(int i)
{
    if (!s_gb.loaded || !s_gb.a_next || i < 0 || i >= s_gb.na) return -1;
    return s_gb.a_next[i];
}

int td5_geob_area_is_plaza(const TD5_GeoArea *a)
{
    if (!a) return 0;
    return a->kind == TD5_GEOA_KIND_PARK || a->kind == TD5_GEOA_KIND_GRASS
        || a->kind == TD5_GEOA_KIND_PITCH || a->kind == TD5_GEOA_KIND_PLAY;
}

/* Shared body of the two point-set tests. The polygon's bounding circle rejects
 * first, so the crossing-number walk only runs where a point could actually be
 * inside; and the whole point set is tested per polygon, so one walk over the
 * span window answers for the caller's entire depth band. */
static int geob_points_hit(int span, const double *px, const double *pz, int np,
                           int win, int areas)
{
    const int lo = (span - win > 0) ? span - win : 0;
    const int hi = (span + win < s_gb.bound_spans - 1)
                 ? span + win : s_gb.bound_spans - 1;
    int s, k;

    if (!s_gb.loaded || s_gb.bound_spans <= 0 || np <= 0) return 0;
    for (s = lo; s <= hi; s++) {
        int i = areas ? td5_geob_span_area(s) : td5_geob_span_building(s);
        for (; i >= 0; i = areas ? td5_geob_next_area(i)
                                 : td5_geob_next_building(i)) {
            double cx, cz, r;
            int first, n;
            if (areas) {
                const TD5_GeoArea *a = &s_gb.a[i];
                if (!td5_geob_area_is_plaza(a)) continue;
                cx = a->cx; cz = a->cz; r = a->radius;
                first = a->first; n = a->n;
            } else {
                const TD5_GeoBuilding *b = &s_gb.b[i];
                cx = b->cx; cz = b->cz; r = b->radius;
                first = b->first; n = b->n;
            }
            for (k = 0; k < np; k++) {
                if (hypot(px[k] - cx, pz[k] - cz) > r) continue;
                if (td5_geob_point_in_ring(&s_gb.px[first], &s_gb.pz[first],
                                           n, px[k], pz[k]))
                    return 1;
            }
        }
    }
    /* [ROUND 1013 F1] a named plaza RING has no polygon to find above. Global,
     * not span-windowed: the hull either holds the point or it does not. */
    if (areas && s_gb.pring_on && s_gb.n_pring > 0) {
        for (k = 0; k < np; k++)
            if (geob_plaza_ring_at(px[k], pz[k]) >= 0) return 1;
    }
    return 0;
}

int td5_geob_points_in_building(int span, const double *px, const double *pz,
                                int np, int win)
{
    return geob_points_hit(span, px, pz, np, win, 0);
}

int td5_geob_points_in_plaza(int span, const double *px, const double *pz,
                             int np, int win)
{
    return geob_points_hit(span, px, pz, np, win, 1);
}

/* ---------------------------------------------------------------- census --- */

void td5_geob_census(int *buildings, int *measured, int *estimated,
                     int *landmarks, int *roofs, int *areas, int *plazas)
{
    int meas = 0, est = 0, lm = 0, roof = 0, plaza = 0, i;
    for (i = 0; i < s_gb.nb; i++) {
        if (s_gb.b[i].hsrc == TD5_GEOB_HSRC_ESTIMATED) est++; else meas++;
        if (s_gb.b[i].landmark) lm++;
        if (s_gb.b[i].roof != TD5_GEOB_ROOF_NONE) roof++;
    }
    for (i = 0; i < s_gb.na; i++)
        if (s_gb.a[i].kind == TD5_GEOA_KIND_PARK
            || s_gb.a[i].kind == TD5_GEOA_KIND_PLAY) plaza++;
    if (buildings) *buildings = s_gb.nb;
    if (measured)  *measured  = meas;
    if (estimated) *estimated = est;
    if (landmarks) *landmarks = lm;
    if (roofs)     *roofs     = roof;
    if (areas)     *areas     = s_gb.na;
    if (plazas)    *plazas    = plaza;
}

void td5_geob_landmark_sources(int *out, int n)
{
    int i;
    if (!out || n <= 0) return;
    for (i = 0; i < n; i++) out[i] = 0;
    for (i = 0; i < s_gb.nb; i++) {
        const int s = s_gb.b[i].lmsrc;
        if (s > 0 && s < n) out[s]++;
    }
}

int td5_geob_place_has_extra(const char *slug)
{
    static char last[64];
    static int  last_val = -1;
    char path[512];
    char *json;
    cJSON *root;
    int v = 0;

    if (!slug || !slug[0]) return 0;
    if (last_val >= 0 && !strcmp(slug, last)) return last_val;
    td5_geo_source_path(path, sizeof(path), slug, "PLACE.JSON");
    json = geob_slurp(path);
    if (json) {
        root = cJSON_Parse(json);
        free(json);
        if (root) {
            const cJSON *src = cJSON_GetObjectItem(root, "sources");
            const cJSON *x = src ? cJSON_GetObjectItem(src, "extra_buildings")
                                 : NULL;
            v = cJSON_IsTrue(x) ? 1 : 0;
            cJSON_Delete(root);
        }
    }
    snprintf(last, sizeof(last), "%s", slug);
    last_val = v;
    return v;
}

void td5_geob_source_census(int *hsrc, int *fsrc)
{
    int i;
    if (hsrc) for (i = 0; i < TD5_GEOB_HSRC_COUNT; i++) hsrc[i] = 0;
    if (fsrc) fsrc[0] = fsrc[1] = 0;
    for (i = 0; i < s_gb.nb; i++) {
        if (hsrc && s_gb.b[i].hsrc < TD5_GEOB_HSRC_COUNT) hsrc[s_gb.b[i].hsrc]++;
        if (fsrc) fsrc[s_gb.b[i].fsrc ? 1 : 0]++;
    }
}

void td5_geob_decimation(int *polys, int *points_dropped)
{
    if (polys)          *polys          = s_gb.dec_polys;
    if (points_dropped) *points_dropped = s_gb.dec_points;
}

/* [ROUND 1009 item 9] The cache's own storey, in world units. See the header:
 * this is the unit a MEASURED height may legitimately be expressed in, as
 * opposed to the facade page's authored cell height, which is a texture
 * property and 1.6x larger. */
double td5_geob_storey_units(void)
{
    if (!s_gb.loaded) return 0.0;
    return s_gb.storey_m * s_gb.units_per_m;
}

double td5_geob_units_per_m(void)
{
    return s_gb.loaded ? s_gb.units_per_m : 0.0;
}
