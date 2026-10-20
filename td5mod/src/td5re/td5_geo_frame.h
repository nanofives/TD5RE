/**
 * td5_geo_frame.h -- THE GEOGRAPHIC -> GAME REFLECTION (PORT-ONLY, roam M, 2026-10-20).
 *
 * WHY. The engine draws its world with screen_x = -vx * focal / vz + cx (td5_render.c): seen
 * from above with +Z up the screen, world +X lies to the LEFT. Every shipped track is authored
 * in that convention. Geographic data was laid in as (x = east, z = north), so La Plata showed
 * MIRRORED, in free roam and on the strip geo race alike (roam HC, _archive/roam/HC/REPORT.md
 * addendum: holding RIGHT turned the car counter-clockwise in real east/north terms).
 *
 * THE RULE. One reflection, where geographic coordinates become game coordinates, before any
 * rotation or offset:
 *
 *     game x = -east        game z = north        game y = up
 *
 * WHERE IT IS APPLIED (nowhere else):
 *   - The SOURCE files (re/assets/geo/<place>/PLACE.JSON projection, tiles/<LAYER>_<ix>_<iz>.JSON "points" /
 *     "x","z", tiles/INDEX.JSON bboxes, HEIGHT.R16 / COVER / WATER / CANOPY rasters) are fetched
 *     data written by re/tools/geo_*.py and stay in the SOURCE frame (x = east). They are never
 *     rewritten.
 *   - FREE ROAM's place frame is the game frame. Its readers convert a parsed source tile with
 *     td5_geo_src_json_to_game (vectors) and td5_geo_src_bbox_to_game (INDEX rows), rasters
 *     come in through gr_raster_read_window_game, and lat/lon goes through
 *     td5_geo_game_x on the east component (td5_roam_latlon_to_place and its few copies).
 *   - The STRIP's route frame comes from lat/lon through gr_proj_to_world, whose route-frame
 *     GeoProj carries `reflect` (td5_geo_route.c). The router still walks the SOURCE frame (it is
 *     mirror-invariant: distances and deflection angles), so the one signed quantity it measures
 *     there, the avenue median SIDE, is carried across with TD5_GEO_GAME_X_SIGN when stored.
 *     A reflected derived PLACE.JSON says so with "reflect_x": 1 in its projection block, and every
 *     reader of a route projection honours it.
 *
 * AFTER THE REFLECTION every side rule is stated in the ENGINE's terms, which are now the real
 * ones: for a heading (tx, tz) the driver's RIGHT is (-tz, tx) and the LEFT is (tz, -tx), both on
 * screen and on the real map. A rule written "right = (tz, -tx), x east" (the old geographic
 * reading) is wrong in the game frame.
 */
#ifndef TD5_GEO_FRAME_H
#define TD5_GEO_FRAME_H

/* game x = TD5_GEO_GAME_X_SIGN * east. A reflection is its own inverse: the same factor maps a
 * game x back to the source frame. */
#define TD5_GEO_GAME_X_SIGN (-1.0)

/* The projection-block key a REFLECTED frame carries (the strip's derived _route/PLACE.JSON): 1 =
 * east is reflected (game x = -east) BEFORE rotation_rad and the offset apply. Absent = a SOURCE
 * frame (x = east), which is what every fetched PLACE.JSON is. */
#define TD5_GEO_PROJ_REFLECT_KEY "reflect_x"

static inline double td5_geo_game_x(double src_x) { return TD5_GEO_GAME_X_SIGN * src_x; }

struct cJSON;
/* Every object of a parsed SOURCE vector file that holds a numeric "x" AND a numeric "z" is
 * carried into the game frame (x negated), at any depth: points[], signals[], nodes[]. */
void td5_geo_src_json_to_game(struct cJSON *root);
/* An INDEX.JSON row's x range [minx, maxx] into the game frame (negated AND swapped). */
void td5_geo_src_bbox_to_game(double *minx, double *maxx);

#endif /* TD5_GEO_FRAME_H */
