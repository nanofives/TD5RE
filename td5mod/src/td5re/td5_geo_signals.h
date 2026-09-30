/**
 * td5_geo_signals.h -- GEO TRACK: traffic-signal nodes + the lamp cycle
 *                      (PORT-ONLY, no original counterpart).
 *
 * TWO HALVES, deliberately in one module because they share one contract --
 * the meaning of the mesh tag the generator writes and the renderer reads.
 *
 *   BUILD TIME.  A reader for re/assets/geo/<slug>/SIGNALS.JSON, the
 *   highway=traffic_signals nodes geo_fetch.py projects into the place frame.
 *   Coordinates are RAW SIGNED WORLD UNITS in the SAME frame as TG_Node and as
 *   ROUTE.JSON's points -- geo_fetch rebuilds the conditioner's projection
 *   (rotation + offset) before converting OSM, so no transform is applied here.
 *   Verified on la_plata: PLACE.JSON offset_x/rotation_rad are byte-identical
 *   to ROUTE.JSON's, and 25 of the 566 nodes land within 30 m of the route.
 *
 *   RUN TIME.  The three lamps of a signal head are additive camera-facing
 *   billboards carrying mesh header tags TD5_MESH_TAG_SIGNAL_LAMP + 0/1/2
 *   (red / amber / green). td5_render_mesh.c reads the tag, asks
 *   td5_geo_signal_lamp_colour() what that lamp should look like right now,
 *   and writes the answer into the transformed vertex diffuse. Nothing else in
 *   the frame changes, which is the whole point: the lights are COSMETIC. No
 *   AI, physics, track or race-state code knows they exist, so a RaceTrace CSV
 *   is identical with and without them.
 *
 * WHY A TAG AND NOT A TEXTURE PAGE. Giving each lamp its own page would need
 * three new TEXTURES.DAT pages, and TD5_TG_PAGE_COUNT is written for EVERY
 * build -- synthetic tracks included -- so the page file would stop being
 * byte-identical and break the standing autotrack gate. The tag costs nothing:
 * it is an int16 the shipped format already carries and every existing writer
 * sets to 0, 1 or 2.
 *
 * WHY TAGS 4..6 ARE SAFE TO CLAIM -- MEASURED, NOT ASSUMED. Every shipped
 * level's models.bin was walked with td5_track_parser.c's own format-A/B
 * autodetect: 31 levels, 20748 mesh headers, tag histogram 0 -> 10948,
 * 1 -> 9247, 2 -> 553, and NOTHING else. So a mesh carrying a tag in this
 * range can only have come from the emitter below, and the render hook needs
 * no second gate to keep shipped tracks out of it.
 */
#ifndef TD5_GEO_SIGNALS_H
#define TD5_GEO_SIGNALS_H

/* Mesh header tag (the int16 at byte offset 2, named texture_page_id in
 * TD5_MeshHeader) for a traffic-light lamp. The existing vocabulary is
 * 0 = opaque, 1 = camera-facing, 2 = camera-facing additive; these continue it.
 * Phase order is TOP DOWN on the head: 0 red, 1 amber, 2 green. */
#define TD5_MESH_TAG_SIGNAL_LAMP  4
#define TD5_GEO_SIGNAL_PHASES     3

/* ----------------------------------------------------------- build time --- */

/* Load SIGNALS.JSON for the place td5_geo_place_slug() reports, unloading any
 * previous one. No place loaded, or no file, leaves the table empty and
 * returns 0 -- never an error, a place is allowed to have no signals.
 * Idempotent: re-calling for the already-loaded slug is a no-op. */
int  td5_geo_signals_sync(void);
void td5_geo_signals_unload(void);

int  td5_geo_signals_count(void);
/* Node i in world units. Returns 0 (and leaves the outputs alone) out of range. */
int  td5_geo_signals_get(int i, double *x, double *z);
/* Path the table came from, "" when nothing is loaded. */
const char *td5_geo_signals_source(void);

/* ------------------------------------------------------------- run time --- */

/* ARGB the lamp of `phase` should draw with at the current wall clock, already
 * scaled for the on/off state. Alpha is left at 0xFF; the pages these lamps use
 * are additive, so "off" is a dim ember rather than a hole. Out-of-range phase
 * returns opaque black. */
unsigned int td5_geo_signal_lamp_colour(int phase);

/* Which phase is lit right now (0 red / 1 amber / 2 green). Diagnostics and
 * tests only -- the renderer goes through td5_geo_signal_lamp_colour. */
int  td5_geo_signal_lamp_phase_now(void);

#endif /* TD5_GEO_SIGNALS_H */
