/**
 * td5_roam_handoff.h -- GEO FREE ROAM: a route handed from the FRONTEND to the next roam session
 * (PORT-ONLY, F11a). See docs/plans/GEO_FREE_ROAM.md (F11 row) and
 * docs/plans/geo_free_roam/F11_PARITY.md.
 *
 * WHY. F11 retires the strip geo BUILD from the menus. The geo selector's route (START, middles,
 * FINISH clicked on the map) and Quick Race's geospatial entry now race IN THE CITY: the roam session
 * spawns at START, and once its planner graph is up this module plants the same waypoints in the
 * route planner (td5_roam_route.h), saves the route by name and then either opens the race setup
 * panel over the route map (the geo selector: the player picks opponents / traffic / cops) or starts
 * the race straight away with a setup the caller filled in (Quick Race: its own opponent count).
 *
 * LIFE CYCLE. td5_roam_handoff_set() after td5_roam_begin(); td5_roam_handoff_frame() runs once per
 * frame from td5_roam_race_frame() and consumes the handoff once (a pause-menu RESTART does not
 * replant it). td5_roam_end() / td5_roam_cancel() drop a pending one. Main thread only.
 */
#ifndef TD5_ROAM_HANDOFF_H
#define TD5_ROAM_HANDOFF_H

#include <stddef.h>

#define TD5_HANDOFF_ROUTE_NAME "geo_map"   /* the saved route the handoff writes (routes/geo_map.json) */

#define TD5_HANDOFF_SETUP_PANEL 0          /* open the race setup panel over the route map */
#define TD5_HANDOFF_AUTOSTART   1          /* start the race with the given opponents / traffic / cops */

/* Waypoints lat/lon, START first and FINISH last (n >= 2, at most TD5_ROUTE_MAX_WP). type =
 * TD5_ROUTE_SPRINT / _CIRCUIT, laps 1..10. opponents / traffic / cops are used by AUTOSTART only
 * (opponents -1 = the session default). Returns 1 when stored. */
int  td5_roam_handoff_set(const double (*ll)[2], int n, int type, int laps, int mode,
                          int opponents, int traffic, int cops);
void td5_roam_handoff_clear(void);
int  td5_roam_handoff_pending(void);
void td5_roam_handoff_frame(void);

/* The last raced city route of a place, for Quick Race: routes/geo_map.json of `place` (written by
 * every handoff; F11b dropped the strip BUILD's _route/ROUTE_RAW.JSON fallback). Fills up to `max`
 * lat/lon pairs.
 * Returns the count (0 = none). `src` (optional) names the file read. */
int  td5_roam_handoff_last_route(const char *place, double (*ll)[2], int max, char *src, size_t src_cap);

/* Read the "waypoints" array of either file format: [[lat, lon], ...] (ROUTE_RAW.JSON) or
 * [{ "lat":..., "lon":... }, ...] (a roam route). Returns the count. */
int  td5_roam_handoff_read_waypoints(const char *path, double (*ll)[2], int max);

/* TD5RE_GEO_STRIP_BUILD=1 (default 0, dev): the STRIP geo BUILD in every context. Without it the
 * strip is local multiplayer only (frontend_geo_strip_context, F11b): a single player's geo selector
 * BUILD races in the city and the GEOSPATIAL entry launches a roam race. */
int  td5_roam_strip_build_enabled(void);

#endif /* TD5_ROAM_HANDOFF_H */
