/**
 * td5_geo_roads.c -- GEO TRACK: real OSM road graph (ROADS.JSON) reader (PORT-ONLY)
 *
 * Contract and rationale in td5_geo_roads.h. Reads the `roads` array
 * re/tools/geo_fetch.py writes into re/assets/geo/<slug>/ROADS.JSON:
 *
 *   {"roads":[{"class":"secondary","lanes":2,"oneway":true,"bridge":false,
 *              "tunnel":false,"layer":0,"width":null,"median":false,
 *              "points":[{"x":257749.23,"z":722919.4}, ...], ...}, ...]}
 *
 * Only the fields the street network needs are kept. `latlon`, `name`,
 * `maxspeed`, `sidewalk`, `median`, `lanes_src` and the raw `tags` sub-object
 * are deliberately skipped: a 2 MB file parses into roughly 10 MB of cJSON DOM
 * and the pool below is the long-lived copy, so it holds the minimum.
 *
 * `surface` USED TO BE ON THAT SKIP LIST and should not have been -- it is on
 * 2238 of La Plata's 2291 ways and it is the one tag here that changes what the
 * driver sees. It is now reduced to a TD5_GEO_SURF_* class (3 values, one int)
 * rather than held as a string, which costs the pool nothing. Same for
 * `junction`, which geo_fetch dropped entirely until 2026-10-07 through a dead
 * `and False` clause, and the direction half of `oneway`. See gate 4 of
 * docs/plans/GEO_TAG_AUDIT.md.
 *
 * WIDTH. OSM tags `lanes` on 45% of ways and `width` (metres, as a free-text
 * string) on 2%; geo_fetch fills the rest from the highway class. Both are
 * reduced HERE to one lane count, because the engine has
 * TD5_TG_SPAN_LENGTH == TD5_TG_LANE_WIDTH and so a street's frontage run is an
 * integer number of lanes -- there is no way to express a 7.3 m street to the
 * mouth table, and pretending otherwise would put the drawn quad and the
 * registered width out of step. A tagged `width` wins over a lane count
 * because it is a measurement rather than a class default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"          /* td5_env_flag_on */
#include "td5_trackgen.h"        /* TD5_TG_LANE_WIDTH */
#include "td5_geo.h"            /* td5_geo_place_path: SOURCE vs DERIVED */
#include "td5_geo_roads.h"
#include "td5_geo_sidewalk.h"   /* [1014 B] the place carriageway table */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

/* Caps. Measured on La Plata (2 200 m radius, a dense regular grid): 2 291
 * roads / 11 739 points, longest way 67 points. Sized for roughly 4x that so a
 * denser centre still loads, and enforced rather than trusted -- an oversized
 * file is truncated with a warning, never allowed to run off the pool. */
#define GEO_ROADS_MAX      8192
#define GEO_ROADS_MAX_PTS  65536
/* A way longer than this is split across records by Overpass anyway; the limit
 * only bounds one walk. */
#define GEO_ROADS_MAX_WAY  512
/* ROADS.JSON for a big city centre is larger than td5_geo.c's 8 MB route/place
 * budget, so this reader carries its own. */
#define GEO_ROADS_MAX_FILE (64 * 1024 * 1024)

/* [ROUND 1011 C3] Interned street names. La Plata's 1937 named ways carry 187
 * distinct names because OSM splits a street at every junction, so the pool is
 * tiny and the dedup is what makes "am I still on the same street?" an integer
 * compare. 512 is roughly 2.7x the measured need. */
#define GEO_ROADS_NAMES_MAX 512

static struct {
    int          n, np;
    TD5_GeoRoad *road;
    double      *px, *pz;
    char       (*name)[TD5_GEO_ROADS_NAME_MAX];
    int          n_names;
    char         slug[64];
    char         source[320];
} s_roads;

/* ------------------------------------------------------------------- io --- */

/* Read a whole file into a malloc'd NUL-terminated buffer, for cJSON. Same
 * shape as td5_geo.c's geo_slurp (which is static there, and carries a smaller
 * cap for the rasters' sidecars). */
static char *geo_roads_slurp(const char *path)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GEO_ROADS_MAX_FILE) {
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

static int geo_roads_class(const char *s)
{
    if (!s || !s[0]) return TD5_GEO_RC_UNKNOWN;
    /* `*_link` ways are ramps onto their parent class and carry its
     * importance, so they collapse onto it rather than onto UNKNOWN. */
    if (!strncmp(s, "motorway", 8))      return TD5_GEO_RC_MOTORWAY;
    if (!strncmp(s, "trunk", 5))         return TD5_GEO_RC_TRUNK;
    if (!strncmp(s, "primary", 7))       return TD5_GEO_RC_PRIMARY;
    if (!strncmp(s, "secondary", 9))     return TD5_GEO_RC_SECONDARY;
    if (!strncmp(s, "tertiary", 8))      return TD5_GEO_RC_TERTIARY;
    if (!strcmp(s, "unclassified"))      return TD5_GEO_RC_UNCLASSIFIED;
    if (!strcmp(s, "residential"))       return TD5_GEO_RC_RESIDENTIAL;
    if (!strcmp(s, "living_street"))     return TD5_GEO_RC_LIVING;
    if (!strcmp(s, "service"))           return TD5_GEO_RC_SERVICE;
    return TD5_GEO_RC_UNKNOWN;
}

/* OSM `surface` -> TD5_GEO_SURF_*. An unrecognised or absent value is SMOOTH,
 * which is the no-op: it takes the span's own biome page, exactly as every
 * street did before this tag was read. Defaulting the OTHER way would turn
 * every untagged street to dirt, which is 299 of La Plata's ways. */
static int geo_roads_surface(const char *s)
{
    static const char *const k_cobble[] = {
        "sett", "cobblestone", "unhewn_cobblestone", "stone", NULL
    };
    static const char *const k_loose[] = {
        "unpaved", "dirt", "ground", "earth", "gravel", "fine_gravel",
        "compacted", "pebblestone", "sand", "mud", "grass", "woodchips", NULL
    };
    int i;
    if (!s || !s[0]) return TD5_GEO_SURF_SMOOTH;
    for (i = 0; k_cobble[i]; i++)
        if (!strcmp(s, k_cobble[i])) return TD5_GEO_SURF_COBBLE;
    for (i = 0; k_loose[i]; i++)
        if (!strcmp(s, k_loose[i])) return TD5_GEO_SURF_LOOSE;
    return TD5_GEO_SURF_SMOOTH;
}

/* [ROUND 1009 item 7] OSM `sidewalk` -> TD5_GEO_SW_*. `separate` means the
 * pavement is mapped as its OWN way, so the footway exists even though this
 * record does not describe it -- which for our purposes is still "there is a
 * pavement here". UNKNOWN (no tag) is deliberately NOT the same as NONE: 2104
 * of La Plata's 2291 ways are untagged and an untagged urban street has a
 * pavement, while `sidewalk=no` is a mapper stating it does not. */
static int geo_roads_sidewalk(const char *s)
{
    if (!s || !s[0]) return TD5_GEO_SW_UNKNOWN;
    if (!strcmp(s, "no") || !strcmp(s, "none"))   return TD5_GEO_SW_NONE;
    if (!strcmp(s, "left"))                       return TD5_GEO_SW_LEFT;
    if (!strcmp(s, "right"))                      return TD5_GEO_SW_RIGHT;
    if (!strcmp(s, "both") || !strcmp(s, "yes")
        || !strcmp(s, "separate"))                return TD5_GEO_SW_BOTH;
    return TD5_GEO_SW_UNKNOWN;
}

/* PAVEMENT WIDTH BY HIGHWAY CLASS, in METRES.
 *
 * These are the widths a street of that class actually has in a laid-out
 * Argentine city centre, which is the place this is measured against: La Plata
 * is a 1882 grid town whose residential veredas are about 2 m, whose avenidas
 * (secondary/primary here) carry 3 m, and whose diagonals and trunk routes the
 * same. A SERVICE way is an alley or a car park aisle and has a kerb at best.
 *
 * They are DEFAULTS, not measurements: nothing in the cache states a pavement
 * width (sidewalk:width is absent on all 2291 ways), so this table is the
 * honest substitute and is stated here once rather than hidden in an emitter. */
double td5_geo_roads_pavement_default_m(int klass)
{
    switch (klass) {
    case TD5_GEO_RC_MOTORWAY:     return 0.0;   /* no pedestrian kerb at all */
    case TD5_GEO_RC_TRUNK:        return 3.0;
    case TD5_GEO_RC_PRIMARY:      return 3.0;
    case TD5_GEO_RC_SECONDARY:    return 3.0;
    case TD5_GEO_RC_TERTIARY:     return 2.5;
    case TD5_GEO_RC_UNCLASSIFIED: return 2.0;
    case TD5_GEO_RC_RESIDENTIAL:  return 2.0;
    case TD5_GEO_RC_LIVING:       return 1.5;
    case TD5_GEO_RC_SERVICE:      return 1.2;
    default:                      return 2.0;
    }
}

static const char *geo_roads_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static int geo_roads_bool(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!v) return 0;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return 0;
}

/* [ROUND 1009 item 7] The per-side sidewalk spellings, which carry the same
 * information split in two: `sidewalk:left=yes` + `sidewalk:right=no` is
 * `sidewalk=left`. Read only when the single `sidewalk` key is absent, so the
 * combined tag stays authoritative where a mapper set both. 20 of La Plata's
 * ways are reachable only this way (left 3, right 17). */
static int geo_roads_sidewalk_sides(const cJSON *r)
{
    const cJSON *tags = r ? cJSON_GetObjectItem(r, "tags") : NULL;
    const char *l = geo_roads_str(tags, "sidewalk:left");
    const char *g = geo_roads_str(tags, "sidewalk:right");
    int have_l, have_r;
    if (!l && !g) return TD5_GEO_SW_UNKNOWN;
    have_l = l && l[0] && strcmp(l, "no") && strcmp(l, "none");
    have_r = g && g[0] && strcmp(g, "no") && strcmp(g, "none");
    if (have_l && have_r) return TD5_GEO_SW_BOTH;
    if (have_l)           return TD5_GEO_SW_LEFT;
    if (have_r)           return TD5_GEO_SW_RIGHT;
    return TD5_GEO_SW_NONE;      /* both spelled out as absent */
}

/* [ROUND 1011 C2] OSM free-text metres -> a number, or 0.
 *
 * The same leading-number read the lane count and `width` already use, pulled
 * out so the four sidewalk:*:width spellings share it. A value that does not
 * start with a number, or that lands outside a plausible pavement, is 0 --
 * "untagged" -- rather than a silently wrong measurement. */
static double geo_roads_tag_m(const cJSON *tags, const char *key,
                              double lo, double hi)
{
    const cJSON *v = tags ? cJSON_GetObjectItem(tags, key) : NULL;
    double m = 0.0;
    if (!v) return 0.0;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        m = strtod(v->valuestring, &end);
        if (end == v->valuestring) return 0.0;
    } else if (cJSON_IsNumber(v)) {
        m = v->valuedouble;
    } else {
        return 0.0;
    }
    return (m > lo && m < hi) ? m : 0.0;
}

/* [ROUND 1011 C2] The MEASURED per-side pavement width, METRES.
 *
 * Most specific first: a side's own `sidewalk:<side>:width` beats the
 * both-sides spellings, and `sidewalk:both:width` beats the bare
 * `sidewalk:width` (which OSM uses for the same "both sides" meaning but which
 * a mapper also reaches for on a one-sided street). A pavement narrower than
 * 0.3 m or wider than 20 m is not a pavement, so it reads as untagged.
 *
 * INERT ON LA PLATA by construction: 0 of the cache's 17909 elements carry any
 * of the four. Kept because the alternative is a reader that cannot be handed a
 * measurement even when one exists. */
static void geo_roads_sidewalk_widths(const cJSON *r, double *l_m, double *r_m)
{
    const cJSON *tags = r ? cJSON_GetObjectItem(r, "tags") : NULL;
    const double both = geo_roads_tag_m(tags, "sidewalk:both:width", 0.3, 20.0);
    const double bare = geo_roads_tag_m(tags, "sidewalk:width", 0.3, 20.0);
    const double common = (both > 0.0) ? both : bare;
    const double l = geo_roads_tag_m(tags, "sidewalk:left:width", 0.3, 20.0);
    const double g = geo_roads_tag_m(tags, "sidewalk:right:width", 0.3, 20.0);
    *l_m = (l > 0.0) ? l : common;
    *r_m = (g > 0.0) ? g : common;
}

/* [ROUND 1011 C2] The street's KIND from its NAME -- see TD5_GEO_NAMEK_* for
 * why this is an int and not a stored string.
 *
 * Prefix only, and case-insensitively, because that is how the name is built:
 * "Avenida 44", "Av. 7", "Diagonal 73", "Calle 15". A name that merely CONTAINS
 * one of these words is not classified by it -- "Pasaje Diagonal" is a pasaje.
 * The English and Portuguese prefixes are there so the rule is not La
 * Plata-only; a place whose table has no row simply never asks. */
int td5_geo_roads_namek_of(const char *s)
{
    static const struct { const char *p; int k; } k_pre[] = {
        { "diagonal",  TD5_GEO_NAMEK_DIAGONAL },
        { "avenida",   TD5_GEO_NAMEK_AVENIDA  },
        { "avenue",    TD5_GEO_NAMEK_AVENIDA  },
        { "boulevard", TD5_GEO_NAMEK_AVENIDA  },
        { "av. ",      TD5_GEO_NAMEK_AVENIDA  },
        { "av ",       TD5_GEO_NAMEK_AVENIDA  },
        { "calle",     TD5_GEO_NAMEK_CALLE    },
        { "street",    TD5_GEO_NAMEK_CALLE    },
        { "rua",       TD5_GEO_NAMEK_CALLE    },
        { "plaza ",    TD5_GEO_NAMEK_PLAZA    },
        { "plazoleta", TD5_GEO_NAMEK_PLAZA    },
        { NULL, 0 }
    };
    int i, k;
    if (!s || !s[0]) return TD5_GEO_NAMEK_UNKNOWN;
    while (*s == ' ') s++;
    for (i = 0; k_pre[i].p; i++) {
        const char *a = s, *b = k_pre[i].p;
        for (k = 0; b[k]; k++) {
            const char c = (a[k] >= 'A' && a[k] <= 'Z') ? (char)(a[k] + 32) : a[k];
            if (c != b[k]) break;
        }
        if (!b[k]) return k_pre[i].k;
    }
    return TD5_GEO_NAMEK_UNKNOWN;
}

/* -------------------------------------- [ROUND 1011 C3] lit / speed / name -- */

/* OSM `lit=*`. The vocabulary is open, but every value that is not an explicit
 * denial means "there is lighting here": `yes`, `24/7`, `automatic`,
 * `sunset-sunrise`, `limited`, `interval`, a lamp count. So the test is the
 * denial, and everything else that is present is YES.
 *
 * MEASURED on La Plata: 1693 `yes`, 0 `no`, 598 untagged. The NO branch is
 * therefore inert on this place and cannot be shown in a framedump here -- it
 * is written because the next cache will have one, and TD5RE_GEO_LIT_FORCE_NO
 * exercises it (see td5_geo_attrs.c). */
static int geo_roads_lit(const char *s)
{
    if (!s || !s[0]) return TD5_GEO_LIT_UNKNOWN;
    if (!strcmp(s, "no") || !strcmp(s, "none") || !strcmp(s, "disused"))
        return TD5_GEO_LIT_NO;
    return TD5_GEO_LIT_YES;
}

/* The implicit-limit table for the `CC:zone` spellings, in km/h.
 *
 * HONESTY NOTE. These are national defaults, not measurements, and this reader
 * normally refuses to invent a number for an untagged way. `CC:urban` is
 * different: it IS a tag, and what it says is "the limit here is this
 * country's urban default" -- resolving it is reading the tag, not guessing.
 * AR is listed explicitly because Argentine urban is 40, not the 50 that most
 * of the world uses, and La Plata is the place this round was built on.
 *
 * INERT ON LA PLATA: the cache carries only the four plain numbers 20/30/40/60
 * and no `CC:zone` spelling at all, so nothing below is reachable there. It is
 * here so the next place does not silently lose its limits. */
static int geo_roads_maxspeed_zone(const char *cc, const char *zone)
{
    const int ar = (cc && (cc[0] == 'A' || cc[0] == 'a')
                       && (cc[1] == 'R' || cc[1] == 'r') && cc[2] == ':');
    if (!strcmp(zone, "walk"))          return 7;
    if (!strcmp(zone, "living_street")) return 20;
    if (!strcmp(zone, "urban"))         return ar ? 40 : 50;
    if (!strcmp(zone, "rural"))         return ar ? 110 : 90;
    if (!strcmp(zone, "trunk"))         return ar ? 120 : 100;
    if (!strcmp(zone, "motorway"))      return ar ? 130 : 110;
    /* GB's national speed limit, the one widespread non-numeric that is not a
     * zone word: 60 and 70 mph. */
    if (!strcmp(zone, "nsl_single"))    return 96;
    if (!strcmp(zone, "nsl_dual"))      return 112;
    return 0;
}

int td5_geo_roads_maxspeed_parse(const char *s)
{
    char *end = NULL;
    double v;
    if (!s || !s[0]) return 0;

    v = strtod(s, &end);
    if (end != s && v > 0.0) {
        /* A unit suffix may follow after optional spaces. OSM's only units are
         * mph and knots; a bare number is km/h by definition of the tag. */
        while (*end == ' ' || *end == '\t') end++;
        if (!strncmp(end, "mph", 3))        v *= 1.609344;
        else if (!strncmp(end, "knots", 5)) return 0;   /* not a road unit */
        if (v < 5.0 || v > 200.0) return 0;             /* junk, not a limit */
        return (int)(v + 0.5);
    }

    /* No leading number. Either an explicit "no limit" (which is not a cap and
     * must not become one) or a `CC:zone` implicit limit. */
    if (!strcmp(s, "none") || !strcmp(s, "signals") || !strcmp(s, "variable")
        || !strcmp(s, "unknown") || !strcmp(s, "default"))
        return 0;
    if (!strcmp(s, "walk")) return 7;
    {
        const char *colon = strchr(s, ':');
        if (colon && colon[1]) return geo_roads_maxspeed_zone(s, colon + 1);
    }
    return 0;
}

/* Copy `s` into `dst`, truncating on a UTF-8 BOUNDARY.
 *
 * snprintf would cut mid-sequence and leave a dangling continuation byte, which
 * the glyph path turns into a replacement box at the end of every long name.
 * The longest name on La Plata is 43 bytes so this never fires there, but a
 * city with longer names must not be the thing that discovers it. */
static void geo_roads_name_copy(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1u;
        /* Walk the cut back off any continuation byte (10xxxxxx). What it then
         * points at is ASCII or a lead byte, and cutting THERE drops that whole
         * sequence rather than half of it. */
        while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* Units per metre of the cache's frame, from PLACE.JSON's cell_units / cell_m.
 * Derived rather than hardcoded at 430 so a cache built at another scale still
 * converts a tagged `width` correctly. 0 means "unknown": the caller then
 * ignores every width tag and uses the lane count, which is exactly the
 * behaviour before this module existed. */
static double geo_roads_units_per_metre(const char *slug)
{
    char path[320];
    char *json;
    double upm = 0.0;
    td5_geo_place_path(path, sizeof path, slug, "PLACE.JSON");
    json = geo_roads_slurp(path);
    if (!json) return 0.0;
    {
        cJSON *root = cJSON_Parse(json);
        const cJSON *cu = root ? cJSON_GetObjectItem(root, "cell_units") : NULL;
        const cJSON *cm = root ? cJSON_GetObjectItem(root, "cell_m") : NULL;
        if (cu && cm && cJSON_IsNumber(cu) && cJSON_IsNumber(cm)
            && cm->valuedouble > 1e-6 && cu->valuedouble > 0.0)
            upm = cu->valuedouble / cm->valuedouble;
        cJSON_Delete(root);
    }
    free(json);
    return upm;
}

/* Lane count for one record: a tagged `width` (metres) when it parses, else the
 * `lanes` field geo_fetch already filled from the tag or the highway class. */
static int geo_roads_lanes(const cJSON *r, double upm)
{
    const cJSON *w = cJSON_GetObjectItem(r, "width");
    const cJSON *l = cJSON_GetObjectItem(r, "lanes");
    int lanes = 2;
    if (l && cJSON_IsNumber(l)) lanes = l->valueint;
    if (upm > 0.0 && w && cJSON_IsString(w) && w->valuestring[0]) {
        /* OSM `width` is free text: "7.5", "6", occasionally "10 m". strtod
         * takes the leading number and ignores the rest; a value that does not
         * start with a number leaves `end == start` and is discarded. */
        char *end = NULL;
        const double m = strtod(w->valuestring, &end);
        if (end != w->valuestring && m > 0.5 && m < 120.0) {
            const int fromw = (int)floor(m * upm / (double)TD5_TG_LANE_WIDTH + 0.5);
            if (fromw >= 1) lanes = fromw;
        }
    }
    if (lanes < 1) lanes = 1;
    if (lanes > TD5_GEO_ROADS_LANES_MAX) lanes = TD5_GEO_ROADS_LANES_MAX;
    return lanes;
}

/* Index of `s` in the name pool, adding it when new. -1 for an empty name or a
 * full pool -- an overflowing pool loses NAMES, never ways. Linear scan: 187
 * entries against 1937 ways is ~360k strcmp once at load, under a millisecond,
 * and a hash here would be state nothing else needs. */
static int geo_roads_intern(const char *s)
{
    int i;
    if (!s || !s[0] || !s_roads.name) return -1;
    for (i = 0; i < s_roads.n_names; i++)
        if (!strcmp(s_roads.name[i], s)) return i;
    if (s_roads.n_names >= GEO_ROADS_NAMES_MAX) return -1;
    geo_roads_name_copy(s_roads.name[s_roads.n_names], TD5_GEO_ROADS_NAME_MAX, s);
    return s_roads.n_names++;
}

void td5_geo_roads_unload(void)
{
    free(s_roads.road);
    free(s_roads.px);
    free(s_roads.pz);
    free(s_roads.name);
    memset(&s_roads, 0, sizeof(s_roads));
}

static int geo_roads_load(const char *slug)
{
    char path[320];
    char *json;
    cJSON *root, *arr;
    const double upm = geo_roads_units_per_metre(slug);
    /* The A/B for the road half of the tag round, same shape as
     * TD5RE_GEO_LM_TAGS and TD5RE_GEO_AREA_TAGS: off, the reader ignores
     * `surface` and `roundabout` and every street takes its biome page, which
     * is what it did before 2026-10-07. */
    const int read_tags = td5_env_flag_on("TD5RE_GEO_ROAD_TAGS");
    int n, i, dropped_short = 0, truncated = 0, roundabouts = 0, n_surf[3];

    td5_geo_roads_unload();
    td5_geo_sw_place(slug);       /* [1014 B] the carriageway table below is the place's */
    td5_geo_place_path(path, sizeof path, slug, "ROADS.JSON");
    json = geo_roads_slurp(path);
    if (!json) {
        TD5_LOG_W(LOG_TAG, "geo: no readable %s; the street network stays synthetic",
                  path);
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geo: %s is not valid JSON", path);
        return 0;
    }
    arr = cJSON_GetObjectItem(root, "roads");
    if (!arr || !cJSON_IsArray(arr) || (n = cJSON_GetArraySize(arr)) < 1) {
        TD5_LOG_E(LOG_TAG, "geo: %s has no roads[]", path);
        cJSON_Delete(root);
        return 0;
    }
    if (n > GEO_ROADS_MAX) { n = GEO_ROADS_MAX; truncated = 1; }

    s_roads.road = (TD5_GeoRoad *)malloc((size_t)n * sizeof(TD5_GeoRoad));
    s_roads.px   = (double *)malloc((size_t)GEO_ROADS_MAX_PTS * sizeof(double));
    s_roads.pz   = (double *)malloc((size_t)GEO_ROADS_MAX_PTS * sizeof(double));
    s_roads.name = (char (*)[TD5_GEO_ROADS_NAME_MAX])
                   malloc((size_t)GEO_ROADS_NAMES_MAX * TD5_GEO_ROADS_NAME_MAX);
    s_roads.n_names = 0;
    if (!s_roads.road || !s_roads.px || !s_roads.pz || !s_roads.name) {
        TD5_LOG_E(LOG_TAG, "geo: out of memory for %d road(s)", n);
        cJSON_Delete(root);
        td5_geo_roads_unload();
        return 0;
    }

    for (i = 0; i < n; i++) {
        const cJSON *r   = cJSON_GetArrayItem(arr, i);
        const cJSON *pts = r ? cJSON_GetObjectItem(r, "points") : NULL;
        const cJSON *cl  = r ? cJSON_GetObjectItem(r, "class") : NULL;
        const cJSON *ly  = r ? cJSON_GetObjectItem(r, "layer") : NULL;
        TD5_GeoRoad *out;
        int k, npt, kept = 0;

        if (!pts || !cJSON_IsArray(pts)) continue;
        npt = cJSON_GetArraySize(pts);
        if (npt < 2) { dropped_short++; continue; }
        if (npt > GEO_ROADS_MAX_WAY) { npt = GEO_ROADS_MAX_WAY; truncated = 1; }
        if (s_roads.np + npt > GEO_ROADS_MAX_PTS) { truncated = 1; break; }

        out = &s_roads.road[s_roads.n];
        memset(out, 0, sizeof(*out));
        out->first = s_roads.np;
        for (k = 0; k < npt; k++) {
            const cJSON *p  = cJSON_GetArrayItem(pts, k);
            const cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
            const cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
            double x, z;
            if (!px || !pz || !cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
            x = px->valuedouble; z = pz->valuedouble;
            /* Drop a repeated vertex: a zero-length segment has no bearing, and
             * the network's junction walk would divide by its length. */
            if (kept > 0) {
                const double dx = x - s_roads.px[out->first + kept - 1];
                const double dz = z - s_roads.pz[out->first + kept - 1];
                if (dx * dx + dz * dz < 1.0) continue;
            }
            s_roads.px[out->first + kept] = x;
            s_roads.pz[out->first + kept] = z;
            if (kept == 0) {
                out->minx = out->maxx = x;
                out->minz = out->maxz = z;
            } else {
                if (x < out->minx) out->minx = x;
                if (x > out->maxx) out->maxx = x;
                if (z < out->minz) out->minz = z;
                if (z > out->maxz) out->maxz = z;
            }
            kept++;
        }
        if (kept < 2) { dropped_short++; continue; }

        out->count  = kept;
        out->klass  = geo_roads_class(cl && cJSON_IsString(cl) ? cl->valuestring : NULL);
        out->lanes  = geo_roads_lanes(r, upm);
        /* [1014 B items 6, 11] THE STREET IS AS WIDE AS THE PLACE'S CARRIAGEWAY.
         * The route reader has applied the place table to the road the car
         * drives since round 1011 (a calle is 10.47 m, 3 lanes) but this reader
         * kept the raw OSM `lanes` -- 2 on 2064 of La Plata's 2291 ways -- so
         * every cross street was drawn 7 m wide against a 10 m road it joins.
         * Same rule as the route's: a TAGGED width wins, a counted lane number
         * is never lowered, the table is a floor. A place with no table row
         * returns 0 and is untouched. TD5RE_GEO_STREET_CARRIAGEWAY=0 for an A/B. */
        if (upm > 0.0 && td5_env_flag_on("TD5RE_GEO_STREET_CARRIAGEWAY")) {
            const cJSON *wd = cJSON_GetObjectItem(r, "width");
            const cJSON *ls = cJSON_GetObjectItem(r, "lanes_src");
            const int tagged = wd && ((cJSON_IsString(wd) && wd->valuestring[0] && atof(wd->valuestring) > 0.5)
                                      || (cJSON_IsNumber(wd) && wd->valuedouble > 0.5));
            const int counted = ls && cJSON_IsString(ls) && ls->valuestring
                             && strcmp(ls->valuestring, "osm_lanes") == 0;
            const int nk = read_tags
                         ? td5_geo_roads_namek_of(geo_roads_str(r, "name"))
                         : TD5_GEO_NAMEK_UNKNOWN;
            const double tm = tagged ? 0.0 : td5_geo_sw_carriageway_m(TD5_GEO_RC_UNKNOWN, nk);
            if (tm > 0.0) {
                int want = (int)floor(tm * upm / (double)TD5_TG_LANE_WIDTH + 0.5);
                if (counted && want < out->lanes) want = out->lanes;
                if (want >= 1 && want <= TD5_GEO_ROADS_LANES_MAX) out->lanes = want;
            }
        }
        out->width  = (double)out->lanes * (double)TD5_TG_LANE_WIDTH;
        out->oneway = geo_roads_bool(r, "oneway");
        out->bridge = geo_roads_bool(r, "bridge");
        out->tunnel = geo_roads_bool(r, "tunnel");
        out->layer  = (ly && cJSON_IsNumber(ly)) ? ly->valueint : 0;
        /* All three absent from a pre-2026-10-07 cache, and all three default
         * to the old behaviour there: SMOOTH, two-way, not a roundabout. */
        out->surface = read_tags
                     ? geo_roads_surface(geo_roads_str(r, "surface")) : 0;
        {
            const cJSON *od = cJSON_GetObjectItem(r, "oneway_dir");
            out->oneway_dir = (od && cJSON_IsNumber(od)) ? od->valueint
                            : (out->oneway ? 1 : 0);
        }
        out->roundabout = read_tags ? geo_roads_bool(r, "roundabout") : 0;
        if (out->roundabout) roundabouts++;
        /* [ROUND 1009 item 7] SIDEWALK + the raw tagged width. Both under
         * `read_tags` with the rest of the 2026-10-07 tag round, so
         * TD5RE_GEO_ROAD_TAGS=0 reproduces the pre-1009 pavement exactly:
         * UNKNOWN on every way, which the consumer reads as "keep the biome
         * width". */
        out->sidewalk = TD5_GEO_SW_UNKNOWN;
        out->tag_width_m = 0.0;
        out->sw_tag_l_m = out->sw_tag_r_m = 0.0;
        /* [1011 C2] The name is read, CLASSIFIED and dropped -- the string
         * never enters the pool. Outside `read_tags` with the rest of the tag
         * round, so TD5RE_GEO_ROAD_TAGS=0 still reproduces the pre-1009 road. */
        out->namek = read_tags
                   ? td5_geo_roads_namek_of(geo_roads_str(r, "name"))
                   : TD5_GEO_NAMEK_UNKNOWN;
        /* [ROUND 1011 C3] lit / maxspeed / name. Under `read_tags` with the
         * rest of the tag round, and all three default to the pre-1011
         * behaviour when it is off: UNKNOWN lighting, no speed cap, no name.
         *
         * `name` and `maxspeed` are TOP-LEVEL fields of the record
         * (geo_fetch.py:1124 and :1157); only `lit` lives inside `tags`
         * (ROAD_TAG_KEYS). The name must be COPIED here: geo_roads_str hands
         * back a pointer into the cJSON DOM and that DOM is deleted below. */
        out->lit          = TD5_GEO_LIT_UNKNOWN;
        out->maxspeed_kph = 0;
        out->name_id      = -1;
        if (read_tags) {
            const cJSON *tags = cJSON_GetObjectItem(r, "tags");
            out->lit = geo_roads_lit(geo_roads_str(tags, "lit"));
            out->maxspeed_kph =
                td5_geo_roads_maxspeed_parse(geo_roads_str(r, "maxspeed"));
            out->name_id = geo_roads_intern(geo_roads_str(r, "name"));

            out->sidewalk = geo_roads_sidewalk(geo_roads_str(r, "sidewalk"));
            if (out->sidewalk == TD5_GEO_SW_UNKNOWN)
                out->sidewalk = geo_roads_sidewalk_sides(r);
            /* [ROUND 1011 C2] the measured per-side width, under the same
             * read_tags gate as the rest of the tag round. */
            geo_roads_sidewalk_widths(r, &out->sw_tag_l_m, &out->sw_tag_r_m);
            {   /* OSM width=* is free text; the same leading-number read the
                 * lane count uses. Kept RAW in metres -- what it means for the
                 * pavement is the consumer's decision, not this reader's. */
                const cJSON *w = cJSON_GetObjectItem(r, "width");
                if (w && cJSON_IsString(w) && w->valuestring[0]) {
                    char *end = NULL;
                    const double m = strtod(w->valuestring, &end);
                    if (end != w->valuestring && m > 0.5 && m < 120.0)
                        out->tag_width_m = m;
                } else if (w && cJSON_IsNumber(w)
                           && w->valuedouble > 0.5 && w->valuedouble < 120.0) {
                    out->tag_width_m = w->valuedouble;
                }
            }
        }
        s_roads.np += kept;
        s_roads.n++;
    }
    cJSON_Delete(root);

    if (s_roads.n < 1) {
        TD5_LOG_E(LOG_TAG, "geo: %s held no usable road", path);
        td5_geo_roads_unload();
        return 0;
    }
    snprintf(s_roads.slug, sizeof s_roads.slug, "%s", slug);
    snprintf(s_roads.source, sizeof s_roads.source, "%s", path);
    TD5_LOG_I(LOG_TAG, "geo: roads %s loaded: %d way(s) %d point(s)%s "
              "(%d too short); scale %.1f units/m",
              path, s_roads.n, s_roads.np, truncated ? " [TRUNCATED at a cap]" : "",
              dropped_short, upm);
    /* The surface census is the attributable half of the surface change: a
     * street that comes out cobbled or loose should be traceable to a count
     * here rather than noticed in a framedump. */
    n_surf[0] = n_surf[1] = n_surf[2] = 0;
    for (i = 0; i < s_roads.n; i++) {
        const int s = s_roads.road[i].surface;
        if (s >= 0 && s < 3) n_surf[s]++;
    }
    TD5_LOG_I(LOG_TAG, "geo: roads surface: %d smooth, %d cobbled, %d loose; "
              "%d roundabout way(s)%s",
              n_surf[0], n_surf[1], n_surf[2], roundabouts,
              read_tags ? "" : " [TD5RE_GEO_ROAD_TAGS=0: tags ignored]");
    /* [ROUND 1011 C3] The attributable census for the three new attributes.
     * A lamp that does not appear, a speed sign that never shows or a blank
     * street blade should be traceable to a count here rather than guessed at
     * from a framedump. Expected on La Plata: 1693 lit yes / 0 no, 1661 speed
     * limits, 187 names. */
    {
        int lit_yes = 0, lit_no = 0, n_speed = 0, n_named = 0;
        for (i = 0; i < s_roads.n; i++) {
            if (s_roads.road[i].lit == TD5_GEO_LIT_YES) lit_yes++;
            else if (s_roads.road[i].lit == TD5_GEO_LIT_NO) lit_no++;
            if (s_roads.road[i].maxspeed_kph > 0) n_speed++;
            if (s_roads.road[i].name_id >= 0) n_named++;
        }
        TD5_LOG_I(LOG_TAG, "geo: roads tags: lit %d yes / %d no / %d untagged; "
                  "maxspeed on %d way(s); %d named way(s) -> %d distinct name(s)"
                  "%s",
                  lit_yes, lit_no, s_roads.n - lit_yes - lit_no, n_speed,
                  n_named, s_roads.n_names,
                  (s_roads.n_names >= GEO_ROADS_NAMES_MAX)
                      ? " [NAME POOL FULL: later names dropped]" : "");
    }
    return 1;
}

int td5_geo_roads_sync(const char *slug)
{
    if (!slug) slug = "";
    if (!slug[0]) {
        if (s_roads.n) td5_geo_roads_unload();
        return 0;
    }
    if (s_roads.n && !strcmp(slug, s_roads.slug)) return 1;
    return geo_roads_load(slug);
}

int         td5_geo_roads_count(void)  { return s_roads.n; }
int         td5_geo_roads_points(void) { return s_roads.np; }
const char *td5_geo_roads_source(void) { return s_roads.n ? s_roads.source : ""; }

const TD5_GeoRoad *td5_geo_roads_get(int i)
{
    return (i >= 0 && i < s_roads.n) ? &s_roads.road[i] : NULL;
}

int td5_geo_roads_point(const TD5_GeoRoad *r, int k, double *x, double *z)
{
    if (!r || k < 0 || k >= r->count) return 0;
    if (x) *x = s_roads.px[r->first + k];
    if (z) *z = s_roads.pz[r->first + k];
    return 1;
}

/* ------------------------------------------------- [1009 item 7] pavement -- */

/* Squared distance from (px,pz) to segment (ax,az)-(bx,bz), and the parameter
 * t of the closest point along it. */
static double geo_roads_seg_d2(double px, double pz, double ax, double az,
                               double bx, double bz, double *t_out)
{
    const double dx = bx - ax, dz = bz - az;
    const double len2 = dx * dx + dz * dz;
    double t = 0.0, cx, cz;
    if (len2 > 1e-9) {
        t = ((px - ax) * dx + (pz - az) * dz) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    cx = ax + dx * t;
    cz = az + dz * t;
    if (t_out) *t_out = t;
    return (px - cx) * (px - cx) + (pz - cz) * (pz - cz);
}

/* [ROUND 1011 C3] THE NEAREST-WAY SEARCH, lifted out of pavement_at unchanged.
 *
 * Byte-for-byte the same loop, the same bbox reject and the same tie-break
 * (strict `d2 >= best2` means the FIRST way at a given distance wins), so
 * pavement_at below returns exactly what it returned before this round. The
 * only addition is `skip_name_id`, which pavement_at passes as -1.
 *
 * Contract and the reason it belongs in a prepass: td5_geo_roads.h. */
const TD5_GeoRoad *td5_geo_roads_nearest(double x, double z, double max_dist,
                                         int skip_name_id,
                                         double *dirx, double *dirz,
                                         double *dist)
{
    const double max2 = max_dist * max_dist;
    double best2 = -1.0, bdx = 0.0, bdz = 1.0;
    const TD5_GeoRoad *best = NULL;
    int i, k;

    if (s_roads.n < 1) return NULL;
    for (i = 0; i < s_roads.n; i++) {
        const TD5_GeoRoad *r = &s_roads.road[i];
        /* Bbox reject, inflated by max_dist so a way whose box misses the
         * point by less than the search radius is still considered. */
        if (x < r->minx - max_dist || x > r->maxx + max_dist) continue;
        if (z < r->minz - max_dist || z > r->maxz + max_dist) continue;
        /* "Not the street I am already on." Compared on the INTERNED id, not
         * the way id, because OSM splits one street into a way per block and a
         * way-id test would call the next block of the same road a cross
         * street. */
        if (skip_name_id >= 0 && r->name_id == skip_name_id) continue;
        for (k = 0; k + 1 < r->count; k++) {
            const double ax = s_roads.px[r->first + k];
            const double az = s_roads.pz[r->first + k];
            const double bx = s_roads.px[r->first + k + 1];
            const double bz = s_roads.pz[r->first + k + 1];
            const double d2 = geo_roads_seg_d2(x, z, ax, az, bx, bz, NULL);
            if (d2 > max2) continue;
            if (best2 >= 0.0 && d2 >= best2) continue;
            {
                const double sx = bx - ax, sz = bz - az;
                const double l = sqrt(sx * sx + sz * sz);
                if (!(l > 1e-6)) continue;
                best2 = d2; best = r; bdx = sx / l; bdz = sz / l;
            }
        }
    }
    if (!best) return NULL;
    if (dirx) *dirx = bdx;
    if (dirz) *dirz = bdz;
    if (dist) *dist = sqrt(best2);
    return best;
}

int td5_geo_roads_name_count(void) { return s_roads.n_names; }

const char *td5_geo_roads_name_by_id(int id)
{
    if (!s_roads.name || id < 0 || id >= s_roads.n_names) return "";
    return s_roads.name[id];
}

const char *td5_geo_roads_name(const TD5_GeoRoad *r)
{
    return r ? td5_geo_roads_name_by_id(r->name_id) : "";
}

int td5_geo_roads_pavement_facts_at(double x, double z, double max_dist,
                                    TD5_GeoPavementAt *out)
{
    double dx = 0.0, dz = 1.0;
    const TD5_GeoRoad *r = td5_geo_roads_nearest(x, z, max_dist, -1, &dx, &dz, NULL);
    if (!r || !out) return 0;
    out->dirx = dx;
    out->dirz = dz;
    out->klass = r->klass;
    out->namek = r->namek;
    out->sidewalk = r->sidewalk;
    out->lanes = r->lanes;
    out->tag_l_m = r->sw_tag_l_m;
    out->tag_r_m = r->sw_tag_r_m;
    /* OSM defines highway `width` as the CARRIAGEWAY's width, so when it is
     * tagged it IS the measurement the frontage rule wants to subtract; the
     * lane count is the fallback, at the same 3.5 m/lane the pre-1011 surplus
     * branch below already assumed. */
    out->half_carriage_m = (r->tag_width_m > 0.0)
                         ? r->tag_width_m * 0.5
                         : (double)r->lanes * TD5_GEO_ROADS_LANE_M * 0.5;
    return 1;
}

int td5_geo_roads_pavement_at(double x, double z, double max_dist,
                              double *left_m, double *right_m,
                              double *dirx, double *dirz)
{
    const TD5_GeoRoad *best =
        td5_geo_roads_nearest(x, z, max_dist, -1, dirx, dirz, NULL);
    if (!best) return 0;

    {
        /* `sidewalk` chooses the SIDES; the class default chooses the WIDTH.
         * NONE still leaves a narrow kerb strip rather than nothing: the
         * facades, the kerb face and the lamp posts all stand on this slab, and
         * a zero width would strand them in the gutter. 0.75 m is a kerb, which
         * is what a street tagged sidewalk=no has. */
        const double def = td5_geo_roads_pavement_default_m(best->klass);
        const double kerb = 0.75;
        double l = def, r = def;
        switch (best->sidewalk) {
        case TD5_GEO_SW_NONE:  l = kerb; r = kerb; break;
        case TD5_GEO_SW_LEFT:  l = def;  r = kerb; break;
        case TD5_GEO_SW_RIGHT: l = kerb; r = def;  break;
        case TD5_GEO_SW_BOTH:  l = def;  r = def;  break;
        default:               break;              /* UNKNOWN: class default */
        }
        /* SPECULATIVE, DEFAULT OFF. OSM defines highway `width` as the
         * CARRIAGEWAY's width, so treating a surplus over the lane count as
         * pavement is an inference, not a measurement -- and on this cache it
         * would fire on 50 of 2291 ways. Behind its own knob so the claim can
         * be tested rather than assumed. */
        if (best->tag_width_m > 0.0
            && td5_env_flag_off("TD5RE_GEO_SW_FROM_WIDTH")) {
            const double carriage = (double)best->lanes * 3.5;
            const double surplus = best->tag_width_m - carriage;
            if (surplus > 2.0) {
                double half = surplus * 0.5;
                if (half > def * 1.5) half = def * 1.5;
                if (half > l) l = half;
                if (half > r) r = half;
            }
        }
        if (left_m)  *left_m  = l;
        if (right_m) *right_m = r;
    }
    return 1;   /* dirx/dirz were written by the nearest-way search above */
}
