/**
 * td5_geo_forks.h -- GEO TRACK: CONFIRMED FORKS for a real place (PORT-ONLY,
 *                    no original counterpart).
 *
 * A reader for re/assets/geo/<slug>/FORKS.JSON, the file the Phase 4 selector
 * writes when the user confirms a fork candidate (a median avenue, or a
 * comparable alternative route) that geo_forks.py detected ALONG THE CHOSEN
 * ROUTE. Nothing here draws anything: the whole module is a table of span
 * ranges that td5_tg_branch.c reads INSTEAD of its own synthetic fork
 * placement when a geo place is loaded.
 *
 * WHY THIS IS A SEPARATE MODULE AND NOT THREE FIELDS IN td5_geo.c. The fork
 * table is consumed by the trackgen's three stateless fork gates
 * (tg_fork_plan, tg_span_in_fork_run, tg_fork_window_ahead) which run during
 * the centreline walk, BEFORE s_forks exists. Giving them one small read-only
 * module with no trackgen dependencies keeps that direction of the dependency
 * graph one-way, and keeps td5_geo.h (already the route + raster contract)
 * from growing a fourth unrelated job.
 *
 * WHAT A FORK ENTRY MEANS. `F` is the ring span where the road SPLITS and
 * `len` the corridor length, exactly as TG_Fork uses them: the fork occupies
 * F .. F+1+len and the corridor is appended after the ring. `kind` is the
 * shape, by the name tg_fork_kind_name() prints, so this module needs no copy
 * of the TG_ForkKind enum and a kind it does not recognise is rejected by name
 * rather than silently becoming fork kind 0.
 *
 * THE LANE CONTRACT, which is the part that is easy to get wrong. The engine
 * refuses a fork whose span at F carries fewer lanes than
 * tg_fork_kind_min_lanes(kind) (4 for an ISLAND), and refuses it again unless
 * the lane count is UNIFORM across F-TD5_TG_BRANCH_WIDEN-2 .. F+1+len+2. A real
 * median avenue arrives from OSM as two one-way ways of 2 lanes each, so the
 * route's own lane count is 2 and every fork would be skipped. The SELECTOR is
 * what fixes that: confirming a fork widens the route to lanes(A)+lanes(B) over
 * the fork's window and re-conditions, so ROUTE.JSON already stores the wider
 * road, and FORKS.JSON's `lanes` is what the selector achieved. This module
 * carries the number so a mismatch is a LOG LINE rather than a fork that
 * vanishes with no explanation.
 *
 * NO EFFECT ON THE SYNTHETIC PATH, by construction: with no place loaded
 * td5_geo_forks_sync() returns 0 before touching the filesystem, and every
 * consumer in td5_tg_branch.c falls straight through to the existing plan
 * ladder. No RNG is drawn here or on the geo fork path, so the standing
 * byte-identity gate (td5_trackgen_internal.h:1290-1296) is untouched.
 */
#ifndef TD5_GEO_FORKS_H
#define TD5_GEO_FORKS_H

/* The engine holds TD5_TG_BRANCH_MAX forks; a file with more is clipped, not
 * rejected, because the extras are only lost detail. Kept as its own name so
 * this header does not have to include td5_trackgen_internal.h. */
#define TD5_GEO_FORKS_MAX 8

/* Load FORKS.JSON for the place td5_geo_place_slug() reports, unloading any
 * previous one. Returns the number of VALID entries (0 when no place is
 * loaded, no file exists, or every entry failed validation -- none of which is
 * an error: a route is allowed to have no confirmed forks). Idempotent, so the
 * trackgen's per-span gates can call it on every query. */
int  td5_geo_forks_sync(void);
void td5_geo_forks_unload(void);

int  td5_geo_forks_count(void);

/* Entry i. `F` is the split span, `len` the corridor length, `sep` the
 * separation scale in [0,1] (small = a divided avenue with a central median),
 * `lanes` the full lane count the selector widened the road to at F. Any
 * output pointer may be NULL. Returns 0 (and leaves the outputs alone) out of
 * range. */
int  td5_geo_forks_get(int i, int *F, int *len, double *sep, int *lanes);

/* The fork's shape, as the name tg_fork_kind_name() prints ("ISLAND",
 * "AVENUE", "WIDE", "SLIP", "MAJOR", "BYPASS"). "" out of range. The caller
 * maps it to TG_ForkKind: the mapping lives with the enum, not here. */
const char *td5_geo_forks_kind(int i);
/* Human label from the selector (the street name), for the log. */
const char *td5_geo_forks_name(int i);

/* Path the table came from, "" when nothing is loaded. */
const char *td5_geo_forks_source(void);

#endif /* TD5_GEO_FORKS_H */
