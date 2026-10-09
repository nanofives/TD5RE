/**
 * td5_geo_attrs.h -- GEO TRACK: the per-span OSM ATTRIBUTE authority (PORT-ONLY)
 *
 * [ROUND 1011 C3] Three attributes that were in ROADS.JSON and used by nothing:
 * `lit` (1693 of La Plata's 2291 ways), `maxspeed` (1661) and `name` (1937 ways
 * resolving to 187 distinct names). This module is the single place that turns
 * them into a per-SPAN answer, so the lamp beat, the sign emitters, the AI cap
 * and the HUD all read the same number and a disagreement is impossible.
 *
 * WHY A SEPARATE MODULE rather than more tables in td5_tg_city.c. Two reasons.
 * The resolution has its own policy (route first, spatial fallback, a default
 * for untagged) that is worth stating once in one file; and the consumers span
 * the generator, the AI and the HUD, which is nobody's existing module.
 *
 * ---------------------------------------------------------------- RESOLUTION
 *
 * `maxspeed` and `name` have TWO possible sources and the order matters:
 *
 *   1. THE ROUTE (td5_geo_route_maxspeed / _name). Authoritative, because the
 *      router knows which way the race actually drives -- at a junction the
 *      nearest way is ambiguous and the route is not. Present only on a
 *      ROUTE.JSON written by round 1011 or later.
 *   2. THE SPATIAL QUERY (td5_geo_roads_nearest). The fallback, so a cache
 *      built before this round still shows limits and names without the user
 *      having to rebuild the route first.
 *
 * `lit` has only the spatial source: lamps stand along side streets too, and
 * the route says nothing about those.
 *
 * ------------------------------------------------------------- UNTAGGED `lit`
 *
 * OSM's `lit=no` is a surveyed statement that a street has no lighting. An
 * ABSENT tag is a statement about the SURVEY, not about the street, and the two
 * must not be conflated. La Plata carries 1693 `yes` and ZERO `no`, so on this
 * place the only observable decision is what an UNTAGGED way does -- 598 of
 * them. The default is NO LAMPS (data-driven and attributable: a lamp you can
 * see traces to a tag). TD5RE_GEO_LIT_DEFAULT=1 lights untagged ways instead,
 * and TD5RE_GEO_LIT_FORCE_NO=1 forces every way to `no`, which is the only way
 * to exercise the NO branch on a cache that has no `no` in it.
 *
 * ------------------------------------------------------------------ LIFETIME
 *
 * td5_geo_attrs_prepare() runs in the generator's single-threaded PREPASS, for
 * the same reason tg_geo_city_prepare does: the spatial query is O(ways) and
 * the downstream emitters run on workers. Everything after it is a read of a
 * frozen table.
 *
 * The RACE-time consumers (the HUD street line, the minimap labels) do NOT use
 * this module -- they read td5_geo_route_name() directly, because a geo track's
 * span index IS its route node index and the route is loaded for every race
 * including a reused cached track that skipped generation entirely.
 *
 * BYTE-IDENTITY. Every entry point returns the "nothing known" answer unless a
 * geo place is loaded, so a synthetic auto track cannot observe this module. It
 * draws no tg_rand/tg_frand and keeps no RNG state.
 */
#ifndef TD5_GEO_ATTRS_H
#define TD5_GEO_ATTRS_H

/* Fill the per-span tables. Safe to call with no geo place loaded (it clears
 * the tables and returns). nspans is clamped to the table size.
 *
 * `nl` is a `const TG_NodeList *`, taken as void so this header stays
 * self-contained: TG_NodeList is an ANONYMOUS struct typedef in
 * td5_trackgen_internal.h, so there is no `struct TG_NodeList` to forward
 * declare and the alternative is dragging the whole generator header into
 * every consumer -- including the HUD. The .c casts it back on the first
 * line. */
void td5_geo_attrs_prepare(const void *nl, int nspans);

/* 1 when a geo place was loaded and the tables hold real answers. */
int  td5_geo_attrs_ready(void);

/* TD5_GEO_LIT_* for the way nearest this span, after the untagged policy
 * above. UNKNOWN out of range or with no place loaded. */
int  td5_geo_attrs_lit(int si);

/* 1 when a lamp belongs at this span: the street is lit AND the span is on the
 * realistic spacing beat (TD5RE_GEO_LAMP_SPACING_M, default 30 m). This is the
 * whole lamp rule on a geo track -- it REPLACES the generator's own 1-in-7
 * beat rather than adding to it. */
int  td5_geo_attrs_lamp_here(int si);

/* Posted limit in km/h for this span, 0 when nothing is known. */
int  td5_geo_attrs_maxspeed(int si);

/* The span where the posted limit CHANGES (so a sign belongs there): returns 1
 * and writes the new limit when si is the first span of a new limit. */
int  td5_geo_attrs_speed_sign_here(int si, int *kph);

/* The street this span is on, "" when unknown. Pointer is owned by this module
 * and valid until the next prepare(). */
const char *td5_geo_attrs_name(int si);
int         td5_geo_attrs_name_id(int si);   /* -1 unknown; module-local ids */

/* The CROSS street at a junction on this span's side, "" when there is none
 * within reach. Excludes every way sharing the race street's name, so the next
 * block of the road you are on is never mistaken for a cross street. */
const char *td5_geo_attrs_cross_name(int si, int left);

/* Census for the log, so a missing lamp or sign is traceable to a count. */
void td5_geo_attrs_report(int nspans);

/* ------------------------------------------------------- the AI speed cap -- */

/* Build the per-span tables at RACE start, from the ROUTE's own nodes.
 *
 * WHY A SECOND ENTRY POINT. The generator's prepass above only runs when a
 * track is GENERATED; a reused cached track skips it entirely, and that is the
 * common case -- you generate a place once and race it repeatedly. The route is
 * loaded for every race either way, and a geo track's span index IS its route
 * node index, so the route alone is enough to rebuild the tables.
 *
 * Cheap when ROUTE.JSON carries the attributes (a table copy). Falls back to
 * the O(spans x ways) spatial query only when it does not, and logs how long
 * that took so the cost is never a mystery.
 *
 * Safe to call on any track: a no-op unless this is a geo slot with a route. */
void td5_geo_attrs_race_init(void);

/* km/h -> ACTOR_LONGITUDINAL_SPEED units (24.8 fixed).
 *
 * NOT an invented constant, and NOT the naive inverse either. The speedo's
 * digit formula is kph = (speed_raw * 256 + 389) / 778 [CONFIRMED @0x438ebc],
 * but `speed_raw` there has already been shifted down by 8 (td5_hud.c
 * recomputes body-frame velocity and does `speed_raw >>= 8`). The actor field
 * the AI reads is the UNSHIFTED 24.8 value, so the /256 and the <<8 cancel and
 * the conversion is just kph * 778.
 *
 * Getting this wrong by that factor of 256 is not a subtle miss: it pinned the
 * AI throttle to its floor on nearly every span. The arithmetic is checkable
 * against constants already in the AI -- 0x4000 and 0xA000 become 21 and
 * 53 km/h, which is what a "moving" and a "quick into this corner" test should
 * read. */
int td5_geo_attrs_kph_to_units(int kph);

/* The speed cap for a span in engine longitudinal_speed units, or 0 for "no
 * cap" -- which is the answer on every shipped track, on the synthetic auto
 * track, where the knob is off, and wherever the map posts no limit.
 *
 * TRAFFIC gets the REAL posted limit: a civilian car obeying the sign is the
 * whole point. A RACER gets the limit times TD5RE_GEO_AI_SPEED_MULT percent
 * (default 250), because an opponent in a racing game that crawled along at
 * 40 km/h on a calle would be a bug, not realism. What the cap buys is the
 * RELATIVE difference -- the AI runs harder on La Plata's 60 km/h avenidas
 * than on its 40 km/h calles, which is what the map actually says. */
int td5_geo_attrs_speed_cap_units(int span, int is_traffic);

#endif /* TD5_GEO_ATTRS_H */
