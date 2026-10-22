/**
 * td5_roam_rules.h -- GEO FREE ROAM: the ROAD RULES of the real map, posted speed limits and turn
 * restrictions (PORT-ONLY, roam LG). See docs/plans/GEO_FREE_ROAM.md; data audit: round K shortlist 8.
 *
 * SPEED LIMITS. OSM `maxspeed` is on 5,133 of the 17,310 La Plata partido ways (casco 1,714), parsed to km/h by
 * td5_geo_roads_maxspeed_parse and carried on every ROAMTILE EDGE record (maxspeed_kph, 0 = untagged).
 * td5_roam_limit_kmh() answers the posted limit where tagged and the CLASS DEFAULT elsewhere: the most common tag of
 * that class on the partido (_archive/roam/LG/maxspeed_census.py), which is also Argentina's urban rule (Ley 24.449:
 * streets 40, avenues 60, motorways 130). Consumers: the planner's time costs (td5_roam_route.c), the traffic's
 * cruise speed (td5_roam_traffic.c), the cops' patrol speed, pursuit cap and speeding notice (td5_roam_cops.c).
 *
 * TURN RESTRICTIONS. re/tools/geo_turn_restrictions.py writes <place>/RESTRICTIONS.JSON (OSM type=restriction
 * relations, source frame). td5_roam_rules_restrictions() reads it ONCE per place (thread safe: the planner graph is
 * built on a worker) into a compact list: kind, from / to way ids, the via node (game frame) or the end points of
 * the via ways. Matching a restriction to a planner-graph node and banning (arm in, arm out) pairs is the planner's
 * (td5_roam_route.c, it owns the graph); the traffic asks td5_roam_route_graph_turn_banned.
 *
 * KNOBS (read by the consumers): TD5RE_ROAM_MAXSPEED (1; 0 = every consumer back to its class table),
 * TD5RE_ROAM_TURN_RESTRICT (1; 0 = no turn banned), TD5RE_ROAM_ROUTE_TIME (1; 0 = the planner costs distance).
 *
 * STATE. One parsed list per place, kept for the process (a few KB). Inert outside a roam session.
 */
#ifndef TD5_ROAM_RULES_H
#define TD5_ROAM_RULES_H

#include <stdint.h>

/* ---- speed limits ------------------------------------------------------------------ */

/* The class default limit, km/h (TD5_GEO_RC_*). */
static inline int td5_roam_class_limit_kmh(int klass)
{
    switch (klass) {
    case 9:  return 130;     /* motorway: 62 of 68 tagged 130                     */
    case 8:  return 60;      /* trunk: 276 of 372 tagged 60 (avenues 1, 7, 44...) */
    case 7:  return 60;      /* primary: 508 of 733                               */
    case 6:  return 60;      /* secondary: 399 of 700                             */
    case 5:  return 40;      /* tertiary: 169 of 297                              */
    case 4:  case 3: return 40;   /* unclassified / residential: 2,398 of 2,587   */
    default: return 20;      /* living street / service / unknown                 */
    }
}

/* The limit of a street: the posted one (maxspeed_kph > 0) or the class default. */
static inline int td5_roam_limit_kmh(int klass, int maxspeed_kph)
{
    return maxspeed_kph > 0 ? maxspeed_kph : td5_roam_class_limit_kmh(klass);
}

/* ---- turn restrictions ----------------------------------------------------------------- */

enum {
    TD5_RESTR_NO_LEFT = 1, TD5_RESTR_NO_RIGHT, TD5_RESTR_NO_STRAIGHT, TD5_RESTR_NO_U,
    TD5_RESTR_NO_ENTRY, TD5_RESTR_NO_EXIT,
    TD5_RESTR_ONLY_LEFT, TD5_RESTR_ONLY_RIGHT, TD5_RESTR_ONLY_STRAIGHT, TD5_RESTR_ONLY_U,
    TD5_RESTR_KIND_COUNT
};

#define TD5_RESTR_WAYS   6
#define TD5_RESTR_VIAPTS 8

typedef struct {
    int64_t id;
    int     kind;                         /* TD5_RESTR_*                                             */
    int     only;                         /* an only_* rule                                          */
    int     nfrom, nto;
    int64_t from[TD5_RESTR_WAYS], to[TD5_RESTR_WAYS];
    int     via_node;                     /* 1 = via is a node at (vx, vz)                           */
    double  vx, vz;                       /* via node, game frame                                    */
    int     nvia_way;                     /* via ways (0 for a via node)                             */
    int64_t via_way[TD5_RESTR_WAYS];
    int     nvpt;                         /* end points of the via ways, game frame                  */
    double  vpx[TD5_RESTR_VIAPTS], vpz[TD5_RESTR_VIAPTS];
} TD5_RoamRestr;

typedef struct {
    int file;                             /* 1 = RESTRICTIONS.JSON was read                          */
    int relations;                        /* records in the file                                     */
    int kept;                             /* bind a car, known kind, from + to + via present         */
    int not_car, unknown_kind, incomplete;/* left out, by reason                                     */
    int by_kind[TD5_RESTR_KIND_COUNT];
} TD5_RoamRestrLoad;

/* The place's restrictions (NULL + 0 when the file is absent). The list lives until the process ends; a call for
 * another place replaces it (the old pointer stays valid: lists are never freed). Thread safe. */
const TD5_RoamRestr *td5_roam_rules_restrictions(const char *place, int *n, TD5_RoamRestrLoad *load);

/* "no_left_turn" etc. for logs. */
const char *td5_roam_rules_kind_name(int kind);

#endif /* TD5_ROAM_RULES_H */
