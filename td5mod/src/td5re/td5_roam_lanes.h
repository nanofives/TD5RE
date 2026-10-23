/**
 * td5_roam_lanes.h -- GEO FREE ROAM: bus-lane / bike-lane paint data (PORT-ONLY, roam PA).
 *
 * <place>/LANES.JSON (re/tools/geo_lane_paint.py): per OSM way id the bus and cycle lane kinds on its
 * left / right (relative to the WAY's direction, as OSM says) and the way's polyline. Read ONCE per place
 * (thread safe: tiles are built on workers). The painter is td5_roam_furniture.c (fu_emit_lanes).
 *
 * KNOB: TD5RE_ROAM_LANES (1; 0 = no lane paint, the old bytes).
 */
#ifndef TD5_ROAM_LANES_H
#define TD5_ROAM_LANES_H

enum { TD5_LANE_NONE = 0, TD5_LANE_LANE = 1, TD5_LANE_TRACK = 2, TD5_LANE_SHARED = 3 };

typedef struct {
    long long id;
    unsigned char bus[2], cyc[2];     /* [0] = left, [1] = right of the way's direction: TD5_LANE_* */
    unsigned char bus_n[2];           /* reserved bus lanes from that kerb */
    unsigned char contraflow;
    int    np;
    const double *px, *pz;            /* the way polyline, GAME frame (place frame, not tile local) */
} TD5_RoamLaneWay;

/* the way's record, or NULL (no LANES.JSON, or the way carries no lane tag). *n_ways (optional) = ways loaded. */
const TD5_RoamLaneWay *td5_roam_lanes_way(const char *place, long long way_id, int *n_ways);

#endif
