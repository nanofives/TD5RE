/**
 * td5_geo_avenues.h -- GEO TRACK: DIVIDED AVENUES for a real place (PORT-ONLY).
 *
 * [ROUND 1010 AVENUES] Mariano, after racing a route he built in the
 * GEOSPATIAL TRACK GENERATOR: "you didn't properly catch avenues of the
 * selected road, you created branches instead of using the actual map".
 *
 * WHY THIS IS NOT td5_geo_forks.h. Round 1009 built a divided avenue through
 * the synthetic fork machinery (TG_FORK_ISLAND): the race road was widened to
 * lanes(A)+lanes(B), split into two drivable carriageways bowing apart, and an
 * island was dropped in the gore between them. That is the wrong shape three
 * times over.
 *
 *   - A FORK IS DRIVABLE. It is a real alternative path -- span records, a
 *     jump table entry and a TG_WO_DRIVABLE paint. You do not drive both sides
 *     of a kerbed median, and the race was getting a branch where the map has
 *     one road with a divider.
 *   - THE WIDTH WAS A CONSTANT. A fork's `sep` is a BOW SCALE, not a distance,
 *     and every avenue was written with sep 0.16 -- TD5_TG_BRANCH_SEP_MIN, the
 *     tightest bow the branch machinery can express -- whether the real
 *     carriageways were 10 m or 21 m apart.
 *   - THE OTHER CARRIAGEWAY WAS INVENTED. The branch's far half was a bowed
 *     copy of the race road, so the real OSM way sitting right there was never
 *     drawn.
 *
 * WHAT AN AVENUE IS INSTEAD. The race drives ITS carriageway with that way's
 * own lanes, unchanged. Beside it, at the offset the REAL map gives, go a
 * kerbed median island and the opposite carriageway's road surface -- geometry
 * only, never a span, so it is scenery that looks like the road it is.
 *
 * THE CONTRACT: re/assets/geo/<slug>/_route/AVENUES.JSON, written by
 * td5_geo_route.c in the same commit as the ROUTE.JSON it is indexed against.
 *
 *   { "place": "la_plata", "spans": 1066, "units_per_metre": 430,
 *     "lane_width": 1500,
 *     "avenues": [ { "id": "median:Diagonal 73:2-9", "name": "Diagonal 73",
 *                    "s0": 48, "s1": 112, "side": 1, "gap_m": 10.8,
 *                    "lanes_opp": 2,
 *                    "spans": [ { "s": 48, "off": -4625.0, "lanes": 2,
 *                                 "open": 0 }, ... ] } ] }
 *
 * `off` is the SIGNED lateral offset from the race centreline to the opposite
 * carriageway's centreline, in WORLD UNITS, positive to the LEFT of travel --
 * the same sign and units tg_road_edge's `shift` takes, so the generator needs
 * no conversion and no frame question. It is a per-SPAN table rather than a
 * copy of the peer polyline because the route conditioner SMOOTHS the
 * centreline: absolute coordinates would drift into (or away from) the race
 * road wherever the smoothing moved it, while an offset measured off the real
 * geometry and applied to the built centreline keeps the median beside the road
 * it divides and still narrows, widens and ends where the OSM ways do.
 *
 * `open` marks a span where a real cross street cuts through the median, so
 * the island leaves a gap for the turn instead of walling it off.
 *
 * ABSENCE IS MEANINGFUL: no file means this route has no divided avenue and the
 * generator builds nothing extra. A missing file is not an error.
 */
#ifndef TD5_GEO_AVENUES_H
#define TD5_GEO_AVENUES_H

/* One route cannot carry more than this many distinct avenues; matches the
 * GR_FORK_MAX the writer caps at. Kept as its own name so this header does not
 * have to include td5_trackgen_internal.h. */
#define TD5_GEO_AVENUES_MAX  8

/* Load (once per place) the avenues of the loaded geo place. Returns the number
 * of avenues kept; 0 on a synthetic build, on a place with no file, or on a
 * file this build refuses. Idempotent and keyed on the slug, like
 * td5_geo_forks_sync. */
int td5_geo_avenues_sync(void);

void td5_geo_avenues_unload(void);

int  td5_geo_avenues_count(void);
const char *td5_geo_avenues_source(void);

/* The span range of avenue `i`, and its name. 0 when `i` is past the table. */
int  td5_geo_avenue_range(int i, int *s0, int *s1, const char **name);

/* Is span `si` inside a divided avenue?
 *
 *   off    signed lateral offset to the opposite carriageway's centreline,
 *          world units, + = left of travel
 *   lanes  that carriageway's own lane count
 *   open   a real cross street cuts the median here
 *
 * Returns 0 (and writes nothing) when the span carries no avenue -- which is
 * also how the table says "the opposite carriageway has ended". */
int  td5_geo_avenue_at(int si, double *off, int *lanes, int *open);

#endif /* TD5_GEO_AVENUES_H */
