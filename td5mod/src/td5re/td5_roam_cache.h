/**
 * td5_roam_cache.h -- GEO FREE ROAM: the cache ROADS reader (PORT-ONLY).
 * See docs/plans/GEO_FREE_ROAM_DESIGN.md section 1.1 (row "Read ways", "Tile
 * files", "Junctions") and section 6 task T1.
 *
 * WHAT IT IS. A session table of the OSM ways in the place's cache tiles, read
 * once per session. The strip reader (td5_geo_roads.c) keeps one pool for one
 * place and drops the way id; the roam reader keeps the id, every attribute the
 * strip keeps, and a 10-unit node key per point, so the road-world builder
 * (F1 T3) can find junctions without re-reading JSON.
 *
 * SELECTION RULE. The same one gr_tiled_load uses (td5_geo_route.c:745): a tile
 * is read when its INDEX.JSON extent meets the window (plus a halo). A way is
 * stored whole in the tile of its first vertex, and the INDEX extent covers the
 * whole way, so a window query is exact for every way that touches it.
 *
 * FRAME. Points are the cache's own world units (the frame of the rasters and
 * of TG_Node). Nothing here rotates or offsets.
 *
 * STATE. One file-scope session table, created by td5_roam_cache_session_begin
 * and freed by td5_roam_cache_session_end. The per-record parse is the lifted
 * td5_geo_roads_parse_record, which is re-entrant. The lane count and the
 * interned names are roam-owned. The place's pavement table (td5_geo_sidewalk.c)
 * is selected by name for the lane floor; the strip selects the same place.
 */
#ifndef TD5_ROAM_CACHE_H
#define TD5_ROAM_CACHE_H

#include <stdint.h>
#include "td5_geo_roads.h"

/* [F1 T3] Raw tag facts the shared record parser does not keep, for the road
 * world's drivable filter and tunnel kind (td5_roam_world.c decides; this only
 * records what the record says). */
#define TD5_ROAM_TAG_HW_BUS_STOP       0x0001u  /* class "bus_stop"            */
#define TD5_ROAM_TAG_HW_RAZED          0x0002u  /* class "razed"               */
#define TD5_ROAM_TAG_HW_SERVICES       0x0004u  /* class "services"            */
#define TD5_ROAM_TAG_HW_RACEWAY        0x0008u  /* class "raceway"             */
#define TD5_ROAM_TAG_AREA              0x0010u  /* tags.area = yes             */
#define TD5_ROAM_TAG_SVC_PARKING_AISLE 0x0020u  /* tags.service = parking_aisle */
#define TD5_ROAM_TAG_SVC_DRIVE_THROUGH 0x0040u  /* tags.service = drive-through */
#define TD5_ROAM_TAG_SVC_DRIVEWAY      0x0080u  /* tags.service = driveway     */
#define TD5_ROAM_TAG_SVC_ALLEY         0x0100u  /* tags.service = alley        */
#define TD5_ROAM_TAG_ACCESS_PRIVATE    0x0200u  /* tags.access = private / no  */
#define TD5_ROAM_TAG_TUNNEL_PASSAGE    0x0400u  /* tunnel_kind building_passage */

/* One way of the session table. road.first / road.count is the slice of the
 * session point pool; road.name_id indexes the session name pool (-1 unnamed). */
typedef struct {
    int64_t    way_id;   /* OSM way id (ROADS record "id"), -1 when absent */
    int        tile;     /* index into the session tile table              */
    uint32_t   tagf;     /* TD5_ROAM_TAG_* bits                            */
    TD5_GeoRoad road;
    /* [F1 T9] The record's own lane count, BEFORE the place floor of
     * td5_roam_lane_count raised it (the strip's GrRoad.lanes_osm), and the two
     * facts the floor read. The graph gives a divided avenue's carriageway its
     * own count back (td5_roam_world.c, "divided carriageways"). */
    int        lanes_osm;
    uint8_t    width_tagged;   /* a measured `width` tag: nothing overrides it */
    uint8_t    lanes_counted;  /* lanes_src = osm_lanes: never lowered         */
} TD5_RoamWay;

/* What one td5_roam_cache_load_window call did. */
typedef struct {
    int      tiles_selected; /* INDEX rows whose extent meets window + halo  */
    int      tiles_read;     /* files parsed by THIS call (0 if all were held) */
    int      ways;           /* ways held by the selected tiles              */
    int      points;         /* points held by the selected tiles            */
    int      names;          /* distinct interned names in the session       */
    uint32_t ms;             /* wall time of the call                        */
} TD5_RoamCacheCensus;

/* Begin a session for `slug`: frees any previous session, reads tiles/INDEX.JSON.
 * Returns the number of ROADS tile rows, or 0 when the place has no readable
 * cache. Call once per place, on the main thread. */
int  td5_roam_cache_session_begin(const char *slug);
void td5_roam_cache_session_end(void);

/* Parse every ROADS tile whose extent meets win = {minx, minz, maxx, maxz} grown
 * by `halo`, each tile at most once per session. Fills *c. Returns the ways in
 * the selected tiles, or -1 when no session is live. */
int  td5_roam_cache_load_window(const double win[4], double halo,
                                TD5_RoamCacheCensus *c);

int  td5_roam_cache_way_count(void);
const TD5_RoamWay *td5_roam_cache_way(int i);

/* Point k of way w: place-frame x/z, and its 10-unit node key. Returns 0 when k
 * is out of range (nothing written). */
int  td5_roam_cache_point(const TD5_RoamWay *w, int k, double *x, double *z,
                          int64_t *node_key);

/* [F1 T5] The interned name behind TD5_GeoRoad.name_id (session pool), NULL for
 * -1 / out of range / no session. The id depends on load order, so a consumer
 * that stores names must re-intern by string (the ROAMTILE writer does). */
const char *td5_roam_cache_name(int id);

/* The 10-unit node key of a place-frame point: (round_even(x/10), round_even(z/10))
 * packed as (kx << 32) | (uint32)kz. Same rule as the router's gr_node_of
 * (GR_SNAP = 10, nearbyint). Pure. */
int64_t td5_roam_node_key(double x, double z);

/* The roam lane count for one record: the strip's carriageway floor (a tagged
 * width wins, a counted lane number is never lowered, the place table is a
 * floor), applied unconditionally. `parsed_lanes` is TD5_GeoRoad.lanes from
 * td5_geo_roads_parse_record. Pure apart from the place table. */
int  td5_roam_lane_count(const struct cJSON *r, double upm, int parsed_lanes,
                         int namek);

#ifndef TD5RE_RELEASE
/* DEV ONLY. When TD5RE_ROAM_CACHE_TEST=1: reads the casco polygon window of the
 * La Plata cache, prints the census (log + <log dir>/roam_cache_census.txt), and
 * checks it against F0a's 2,170 polygon-touching ways. Returns 1 when the test
 * ran (the caller should then exit), 0 otherwise. */
int  td5_roam_cache_dev_census(void);
#endif

#endif /* TD5_ROAM_CACHE_H */
