/**
 * td5_geo_attrs.c -- GEO TRACK: the per-span OSM ATTRIBUTE authority (PORT-ONLY)
 *
 * Contract, resolution order and the untagged-`lit` policy are all stated in
 * td5_geo_attrs.h. This file is the mechanism.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_buildings.h"       /* td5_geob_units_per_m */
#include "td5_trackgen.h"
#include "td5_trackgen_internal.h"   /* TG_NodeList / TG_Node */
#include "td5_geo_attrs.h"

/* td5_trackgen_internal.h sets its own LOG_TAG. These lines belong beside the
 * other geo census output, so take it back. */
#undef  LOG_TAG
#define LOG_TAG "geo"

/* Search radius for the nearest mapped way, in world units. The same 25 m
 * tg_geo_city_prepare uses and for the same reason: the conditioner rotates
 * and resamples the real drive, so the generated centreline sits a few metres
 * off the OSM polyline, and 25 m is past that while staying well inside a
 * La Plata block (120 m). */
#define GA_SEEK        10750.0

/* How far off the centre line to probe for a CROSS street. Past the
 * carriageway and the pavement, inside the block. */
#define GA_CROSS_OUT   6450.0        /* 15 m */

/* Interned names, module-local ids. A route touches 18 streets and its cross
 * streets add a few dozen more. */
#define GA_NAMES_MAX   128

static struct {
    int            ready;
    int            nspans;
    unsigned char *lit;        /* TD5_GEO_LIT_*                      */
    unsigned char *lamp;       /* 1 = a lamp belongs on this span    */
    short         *kph;        /* 0 = unknown                        */
    short         *name;       /* -1 = unknown, else a GA id         */
    short         *cross[2];   /* [0]=left [1]=right, -1 = none      */
    char         (*names)[TD5_GEO_ROADS_NAME_MAX];
    int            n_names;
    /* census */
    long           n_lit_yes, n_lit_no, n_lit_unk;
    long           n_lamp, n_speed_from_route, n_speed_from_roads;
    long           n_name_from_route, n_name_from_roads, n_cross;
    int            lamp_every;     /* the beat actually used, in spans */
} s_ga;

/* Which track index the race tables were last built for. Keyed on the index,
 * not a bool, so race 2 on a different track can never read race 1's tables.
 * -2 is "never tried" (-1 is a legitimate track index during boot). */
static int s_race_tried_track = -2;

/* ----------------------------------------------------------------- names -- */

static int ga_intern(const char *s)
{
    int i;
    if (!s_ga.names || !s || !s[0]) return -1;
    for (i = 0; i < s_ga.n_names; i++)
        if (!strcmp(s_ga.names[i], s)) return i;
    if (s_ga.n_names >= GA_NAMES_MAX) return -1;
    /* Truncate on a UTF-8 boundary -- half a sequence draws as a replacement
     * box. Same rule as the two other interning sites. */
    {
        size_t n = strlen(s);
        if (n >= TD5_GEO_ROADS_NAME_MAX) {
            n = TD5_GEO_ROADS_NAME_MAX - 1u;
            while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;
        }
        memcpy(s_ga.names[s_ga.n_names], s, n);
        s_ga.names[s_ga.n_names][n] = '\0';
    }
    return s_ga.n_names++;
}

static void ga_free(void)
{
    free(s_ga.lit); free(s_ga.lamp); free(s_ga.kph); free(s_ga.name);
    free(s_ga.cross[0]); free(s_ga.cross[1]); free(s_ga.names);
    memset(&s_ga, 0, sizeof(s_ga));
}

/* --------------------------------------------------------------- prepass -- */

void td5_geo_attrs_prepare(const void *nlv, int nspans)
{
    const TG_NodeList *nl = (const TG_NodeList *)nlv;
    double upm, spacing_m;
    int si, force_no, untagged_lit;

    ga_free();

    /* Every branch below is past this gate, which is what keeps a synthetic
     * auto track byte-identical: with no place loaded there are no roads, the
     * tables stay NULL and every accessor returns "nothing known". */
    /* Every bail says WHY. A silent early-out here is indistinguishable from
     * "the attributes are all unknown", and that ambiguity cost one run. */
    if (!nl || nspans < 1) {
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] off: no node list (nspans=%d)", nspans);
        return;
    }
    if (!td5_geo_loaded()) {
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] off: no geo place loaded");
        return;
    }
    if (!td5_env_flag_on("TD5RE_GEO_ATTRS")) {
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] off: TD5RE_GEO_ATTRS=0");
        return;
    }
    if (td5_geo_roads_count() < 1) {
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] off: no roads loaded");
        return;
    }

    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    s_ga.lit      = (unsigned char *)calloc((size_t)nspans, 1);
    s_ga.lamp     = (unsigned char *)calloc((size_t)nspans, 1);
    s_ga.kph      = (short *)calloc((size_t)nspans, sizeof(short));
    s_ga.name     = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.cross[0] = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.cross[1] = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.names    = (char (*)[TD5_GEO_ROADS_NAME_MAX])
                    malloc((size_t)GA_NAMES_MAX * TD5_GEO_ROADS_NAME_MAX);
    if (!s_ga.lit || !s_ga.lamp || !s_ga.kph || !s_ga.name
        || !s_ga.cross[0] || !s_ga.cross[1] || !s_ga.names) {
        TD5_LOG_E(LOG_TAG, "[GEO ATTRS] out of memory for %d span(s)", nspans);
        ga_free();
        return;
    }
    for (si = 0; si < nspans; si++)
        s_ga.name[si] = s_ga.cross[0][si] = s_ga.cross[1][si] = -1;
    s_ga.nspans = nspans;

    force_no     = td5_env_flag_off("TD5RE_GEO_LIT_FORCE_NO");
    untagged_lit = td5_env_flag_off("TD5RE_GEO_LIT_DEFAULT");

    /* The lamp beat, in SPANS, derived from a real spacing in metres rather
     * than from a span count -- 30 m is ordinary urban street lighting, and a
     * span is TD5_TG_SPAN_LENGTH / units_per_metre long (3.5 m on La Plata's
     * 430 units/m), so the beat is ~9 spans there. Clamped so a bad
     * units_per_metre cannot produce a lamp every span or none at all. */
    upm = td5_geob_units_per_m();
    if (!(upm > 1.0)) upm = 430.0;
    spacing_m = (double)td5_env_int("TD5RE_GEO_LAMP_SPACING_M", 30, 8, 200);
    s_ga.lamp_every = (int)(spacing_m * upm / (double)TD5_TG_SPAN_LENGTH + 0.5);
    if (s_ga.lamp_every < 1)  s_ga.lamp_every = 1;
    if (s_ga.lamp_every > 64) s_ga.lamp_every = 64;

    for (si = 0; si < nspans; si++) {
        const TG_Node *n = &nl->v[si];
        const TD5_GeoRoad *r =
            td5_geo_roads_nearest(n->x, n->z, GA_SEEK, -1, NULL, NULL, NULL);
        int lit = r ? r->lit : TD5_GEO_LIT_UNKNOWN;
        int kph, nm;

        if (force_no) lit = TD5_GEO_LIT_NO;
        s_ga.lit[si] = (unsigned char)lit;
        if (lit == TD5_GEO_LIT_YES)     s_ga.n_lit_yes++;
        else if (lit == TD5_GEO_LIT_NO) s_ga.n_lit_no++;
        else                            s_ga.n_lit_unk++;

        /* A lamp stands where the street is lit AND the beat falls. An
         * UNTAGGED street is unlit unless TD5RE_GEO_LIT_DEFAULT says
         * otherwise: a lamp you can see should trace to a tag. */
        {
            const int lit_here = (lit == TD5_GEO_LIT_YES)
                              || (lit == TD5_GEO_LIT_UNKNOWN && untagged_lit);
            if (lit_here && (si % s_ga.lamp_every) == 0) {
                s_ga.lamp[si] = 1;
                s_ga.n_lamp++;
            }
        }

        /* THE ROUTE FIRST. At a junction the nearest way is ambiguous and the
         * route is not -- it knows which way the race actually drives. The
         * spatial answer is the fallback for a cache whose ROUTE.JSON predates
         * this round. */
        kph = td5_geo_route_maxspeed(si);
        if (kph > 0) s_ga.n_speed_from_route++;
        else if (r && r->maxspeed_kph > 0) {
            kph = r->maxspeed_kph;
            s_ga.n_speed_from_roads++;
        }
        s_ga.kph[si] = (short)kph;

        nm = -1;
        {
            const char *rn = td5_geo_route_name(si);
            if (rn && rn[0]) { nm = ga_intern(rn); s_ga.n_name_from_route++; }
            else if (r) {
                const char *sn = td5_geo_roads_name(r);
                if (sn && sn[0]) { nm = ga_intern(sn); s_ga.n_name_from_roads++; }
            }
        }
        s_ga.name[si] = (short)nm;

        /* THE CROSS STREET, probed once per side out past the carriageway.
         * `skip_name_id` is the race street's id in the ROADS pool, so the next
         * block of the road being driven -- OSM splits it at every junction --
         * can never come back as the cross street. */
        if (r) {
            const double tx = n->tx, tz = n->tz;
            int side;
            for (side = 0; side < 2; side++) {
                /* left of travel is (-tz, tx) with this frame's handedness;
                 * side 0 = left, side 1 = right. */
                const double s = (side == 0) ? 1.0 : -1.0;
                const double px = n->x + (-tz) * GA_CROSS_OUT * s;
                const double pz = n->z + ( tx) * GA_CROSS_OUT * s;
                const TD5_GeoRoad *c =
                    td5_geo_roads_nearest(px, pz, GA_CROSS_OUT, r->name_id,
                                          NULL, NULL, NULL);
                const char *cn = c ? td5_geo_roads_name(c) : NULL;
                if (cn && cn[0]) {
                    s_ga.cross[side][si] = (short)ga_intern(cn);
                    if (s_ga.cross[side][si] >= 0) s_ga.n_cross++;
                }
            }
        }
    }

    s_ga.ready = 1;
}

/* ------------------------------------------------------ race-time rebuild -- */

/* The same tables, rebuilt at RACE start from the ROUTE's own nodes rather
 * than the generator's node list. Rationale in td5_geo_attrs.h: a reused
 * cached track never runs the generator's prepass, and that is the ordinary
 * case once a place has been built.
 *
 * The tangent is taken from consecutive route nodes, which is what tg_geo_walk
 * would have produced anyway -- it pushes route node i verbatim as span i. */
void td5_geo_attrs_race_init(void)
{
    const int n = td5_geo_route_count();
    int nspans, si, route_has_speed = 0, route_has_name = 0;
    int64_t t0;

    /* The generator may already have filled these in this very process, with
     * the richer node-list tangents. Don't throw that away. */
    if (s_ga.ready && s_ga.nspans >= n - 1) {
        s_race_tried_track = g_td5.track_index;
        return;
    }

    ga_free();
    if (n < 2) return;
    if (!td5_geo_loaded()) return;
    if (!td5_env_flag_on("TD5RE_GEO_ATTRS")) return;
    if (!td5_trackgen_is_geo_slot(g_td5.track_index)) {
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] race: not a geo slot (track %d)",
                  g_td5.track_index);
        return;
    }

    for (si = 0; si < n; si++) {
        if (td5_geo_route_maxspeed(si) > 0)  route_has_speed = 1;
        if (td5_geo_route_name_id(si) >= 0)  route_has_name = 1;
        if (route_has_speed && route_has_name) break;
    }

    /* Only pay for the roads layer when the route cannot answer on its own.
     * Syncing it is idempotent -- the minimap may already have done it. */
    if (!route_has_speed || !route_has_name)
        td5_geo_roads_sync(td5_geo_place_slug());

    nspans = n - 1;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    s_ga.lit      = (unsigned char *)calloc((size_t)nspans, 1);
    s_ga.lamp     = (unsigned char *)calloc((size_t)nspans, 1);
    s_ga.kph      = (short *)calloc((size_t)nspans, sizeof(short));
    s_ga.name     = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.cross[0] = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.cross[1] = (short *)malloc((size_t)nspans * sizeof(short));
    s_ga.names    = (char (*)[TD5_GEO_ROADS_NAME_MAX])
                    malloc((size_t)GA_NAMES_MAX * TD5_GEO_ROADS_NAME_MAX);
    if (!s_ga.lit || !s_ga.lamp || !s_ga.kph || !s_ga.name
        || !s_ga.cross[0] || !s_ga.cross[1] || !s_ga.names) {
        TD5_LOG_E(LOG_TAG, "[GEO ATTRS] race: out of memory for %d span(s)",
                  nspans);
        ga_free();
        return;
    }
    for (si = 0; si < nspans; si++)
        s_ga.name[si] = s_ga.cross[0][si] = s_ga.cross[1][si] = -1;
    s_ga.nspans = nspans;
    s_ga.lamp_every = 0;      /* lamps are a GENERATION decision, not a race one */

    t0 = td5_plat_time_us();
    for (si = 0; si < nspans; si++) {
        double x = 0.0, z = 0.0;
        const TD5_GeoRoad *r = NULL;
        int kph = td5_geo_route_maxspeed(si);
        const char *nm = td5_geo_route_name(si);

        td5_geo_route_node(si, &x, &z, NULL);
        if (kph <= 0 || !nm || !nm[0])
            r = td5_geo_roads_nearest(x, z, GA_SEEK, -1, NULL, NULL, NULL);

        if (kph > 0) s_ga.n_speed_from_route++;
        else if (r && r->maxspeed_kph > 0) {
            kph = r->maxspeed_kph;
            s_ga.n_speed_from_roads++;
        }
        s_ga.kph[si] = (short)kph;

        if (nm && nm[0]) {
            s_ga.name[si] = (short)ga_intern(nm);
            s_ga.n_name_from_route++;
        } else if (r) {
            const char *sn = td5_geo_roads_name(r);
            if (sn && sn[0]) {
                s_ga.name[si] = (short)ga_intern(sn);
                s_ga.n_name_from_roads++;
            }
        }
    }

    s_ga.ready = 1;
    TD5_LOG_I(LOG_TAG, "[GEO ATTRS] race: %d span(s) in %.1f ms "
              "(route carries speed=%d name=%d); maxspeed %ld route / %ld "
              "nearest-way; %d distinct name(s)",
              nspans, (double)(td5_plat_time_us() - t0) / 1000.0,
              route_has_speed, route_has_name,
              s_ga.n_speed_from_route, s_ga.n_speed_from_roads, s_ga.n_names);
}

int td5_geo_attrs_kph_to_units(int kph)
{
    if (kph <= 0) return 0;
    return (int)(((long)kph * 778L) / 256L);
}

/* Build the race tables on first use, once per track index.
 *
 * SELF-TRIGGERING ON PURPOSE. The alternative is a call in the race-init path,
 * which lives in td5_game.c / td5_track.c -- and keying the latch on
 * g_td5.track_index is also what makes this immune to the stale-state class of
 * bug where race 2 on a different track reads race 1's tables. A track index
 * that produced nothing is remembered too, so a shipped track does not retry
 * the lookup on every AI tick. */
static void ga_race_ensure(void)
{
    if (s_race_tried_track == g_td5.track_index) return;
    s_race_tried_track = g_td5.track_index;
    td5_geo_attrs_race_init();
}

int td5_geo_attrs_speed_cap_units(int span, int is_traffic)
{
    static int s_mult = -1;
    int kph;
    ga_race_ensure();
    if (!s_ga.ready) return 0;
    if (span < 0 || span >= s_ga.nspans) return 0;
    kph = (int)s_ga.kph[span];
    if (kph <= 0) return 0;
    if (s_mult < 0)
        s_mult = td5_env_int("TD5RE_GEO_AI_SPEED_MULT", 250, 100, 1000);
    if (!is_traffic) kph = (kph * s_mult) / 100;
    return td5_geo_attrs_kph_to_units(kph);
}

/* --------------------------------------------------------------- queries -- */

int td5_geo_attrs_ready(void) { return s_ga.ready; }

static int ga_in(int si) { return s_ga.ready && si >= 0 && si < s_ga.nspans; }

int td5_geo_attrs_lit(int si)
{
    return ga_in(si) ? (int)s_ga.lit[si] : TD5_GEO_LIT_UNKNOWN;
}

int td5_geo_attrs_lamp_here(int si)
{
    return ga_in(si) ? (int)s_ga.lamp[si] : 0;
}

int td5_geo_attrs_maxspeed(int si)
{
    return ga_in(si) ? (int)s_ga.kph[si] : 0;
}

int td5_geo_attrs_speed_sign_here(int si, int *kph)
{
    int here;
    if (!ga_in(si)) return 0;
    here = (int)s_ga.kph[si];
    if (here <= 0) return 0;
    /* A sign belongs where the limit CHANGES. Span 0 counts as a change: the
     * driver has to be told the limit once before it can change. */
    if (si > 0 && (int)s_ga.kph[si - 1] == here) return 0;
    if (kph) *kph = here;
    return 1;
}

int td5_geo_attrs_name_id(int si)
{
    return ga_in(si) ? (int)s_ga.name[si] : -1;
}

const char *td5_geo_attrs_name(int si)
{
    const int id = td5_geo_attrs_name_id(si);
    if (!s_ga.names || id < 0 || id >= s_ga.n_names) return "";
    return s_ga.names[id];
}

const char *td5_geo_attrs_cross_name(int si, int left)
{
    const int side = left ? 0 : 1;
    int id;
    if (!ga_in(si) || !s_ga.cross[side]) return "";
    id = (int)s_ga.cross[side][si];
    if (!s_ga.names || id < 0 || id >= s_ga.n_names) return "";
    return s_ga.names[id];
}

void td5_geo_attrs_report(int nspans)
{
    (void)nspans;
    if (!s_ga.ready) return;
    TD5_LOG_I(LOG_TAG, "[GEO ATTRS] %d span(s): lit %ld yes / %ld no / %ld "
              "untagged -> %ld lamp span(s) every %d span(s)",
              s_ga.nspans, s_ga.n_lit_yes, s_ga.n_lit_no, s_ga.n_lit_unk,
              s_ga.n_lamp, s_ga.lamp_every);
    TD5_LOG_I(LOG_TAG, "[GEO ATTRS] maxspeed: %ld span(s) from the ROUTE, "
              "%ld from the nearest way, %ld unknown",
              s_ga.n_speed_from_route, s_ga.n_speed_from_roads,
              (long)s_ga.nspans - s_ga.n_speed_from_route
                  - s_ga.n_speed_from_roads);
    TD5_LOG_I(LOG_TAG, "[GEO ATTRS] names: %ld span(s) from the ROUTE, %ld "
              "from the nearest way; %ld cross-street blade(s); %d distinct "
              "name(s)%s",
              s_ga.n_name_from_route, s_ga.n_name_from_roads, s_ga.n_cross,
              s_ga.n_names,
              (s_ga.n_names >= GA_NAMES_MAX) ? " [NAME POOL FULL]" : "");
    /* The limits actually in play, which is what a speed sign has to depict. */
    {
        int si, prev = -1;
        char line[256];
        size_t used = 0;
        line[0] = '\0';
        for (si = 0; si < s_ga.nspans; si++) {
            const int k = (int)s_ga.kph[si];
            if (k == prev) continue;
            prev = k;
            if (k <= 0) continue;
            if (used + 16u >= sizeof line) break;
            used += (size_t)snprintf(line + used, sizeof line - used,
                                     "%s%d@%d", used ? ", " : "", k, si);
        }
        TD5_LOG_I(LOG_TAG, "[GEO ATTRS] limit changes (km/h@span): %s",
                  line[0] ? line : "none");
    }
}
