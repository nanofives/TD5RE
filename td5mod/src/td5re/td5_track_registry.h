/**
 * td5_track_registry.h -- runtime registry for custom (user-built) tracks.
 *
 * Loads re/assets/levels/custom_tracks.json at boot (written by
 * re/tools/td5_trackgen.py) so a custom track becomes selectable in the
 * frontend and AutoRace-able WITHOUT recompiling per track.
 *
 * Custom tracks occupy frontend schedule slots at/above
 * TD5_CUSTOM_TRACK_SLOT_BASE (37) -- past the 0-19 native tracks, the 20-25
 * championship cups, and the 26-36 migrated TD6 tracks. Each manifest entry
 * maps a slot to its levelNNN/ directory and per-track parameters.
 *
 * The frontend (track name, selector bound, lock state) queries by SLOT;
 * the asset loader (level number, finish span, sky pitch) queries by LEVEL.
 */
#ifndef TD5_TRACK_REGISTRY_H
#define TD5_TRACK_REGISTRY_H

/* First frontend schedule slot reserved for custom tracks. The native +
 * cup + TD6 tables occupy 0-36; custom tracks start here. */
#define TD5_CUSTOM_TRACK_SLOT_BASE 37
/* Maximum number of custom tracks loadable from the manifest. Sizes the
 * frontend lock table headroom (s_track_lock_table) so custom slots are
 * always in-bounds and read as unlocked. */
#define TD5_CUSTOM_TRACK_MAX 24

/* Module lifecycle (registered in g_td5re_modules, after "track"). */
int  td5_track_registry_init(void);
void td5_track_registry_shutdown(void);

/* Count of custom tracks loaded from the manifest (0 if none / no manifest). */
int  td5_track_registry_count(void);

/* Exclusive upper bound of custom slots: (highest custom slot + 1), or
 * TD5_CUSTOM_TRACK_SLOT_BASE when no custom tracks are loaded. Used to extend
 * the frontend track-cycler's range. */
int  td5_track_registry_slot_max(void);

/* --- by SLOT (frontend) --- */
int          td5_track_registry_has_slot(int slot);
const char  *td5_track_registry_name_for_slot(int slot);   /* NULL if not custom */
int          td5_track_registry_level_for_slot(int slot);  /* <=0 if not custom */
int          td5_track_registry_tga_for_slot(int slot);    /* -1 = no preview */

/* --- by LEVEL (asset loader) --- */
int   td5_track_registry_has_level(int level_num);
int   td5_track_registry_is_circuit_for_level(int level_num);  /* 1/0; -1 unknown */
int   td5_track_registry_start_span_for_level(int level_num);  /* -1 unknown */
int   td5_track_registry_finish_span_for_level(int level_num); /* -1 unknown */
float td5_track_registry_sky_pitch_for_level(int level_num);   /* <0 unknown */

#endif /* TD5_TRACK_REGISTRY_H */
