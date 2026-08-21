/**
 * td5_trackgen.h -- procedural ("AUTO-GENERATED") track builder (PORT-ONLY).
 *
 * Phase 1: at race launch, roll a seed and synthesise a complete, finite track
 * into a reserved loose level directory (re/assets/levels/levelNNN/), then
 * register it so the normal td5_asset_load_level path picks it up. Every
 * downstream system (AI routes, span walker, minimap, lap/finish, camera) sees
 * an ordinary point-to-point track and needs no changes.
 *
 * This is a C port of the geometry+emitter half of re/tools/td5_trackgen.py.
 * It deliberately writes the BINARY level entries (STRIP.DAT, LEFT/RIGHT.TRK,
 * LEVELINF.DAT) rather than the editable-source JSON/CSV the Python tool emits,
 * so no pack-on-load round-trip is needed at race launch.
 *
 * Coordinates are raw signed world units (NOT 24.8) -- the renderer divides by
 * 256. One lane is TD5_TG_LANE_WIDTH world units wide.
 *
 * Phase 2 (not implemented here): mid-race streaming -- append/unload spans as
 * the player progresses. That needs the int16 span-index rebase, route-table
 * realloc, a sliding minimap and procedural scenery; see the module comment in
 * td5_trackgen.c for why Phase 1 is finite instead.
 */
#ifndef TD5_TRACKGEN_H
#define TD5_TRACKGEN_H

/* One lane's width in world units (matches td5_trackgen.py's lane_width). */
#define TD5_TG_LANE_WIDTH   1500
/* Down-track distance between consecutive spans, world units. */
#define TD5_TG_SPAN_LENGTH  1500

/* Section archetypes the picker chooses between. Weights are relative
 * (any non-negative ints); the picker normalises them, so they can be read as
 * percentages when they happen to sum to 100. */
typedef enum {
    TD5_TG_STRAIGHT = 0,   /* constant heading */
    TD5_TG_CURVE,          /* sweeping bend, wide radius */
    TD5_TG_ACUTE,          /* tight/hairpin bend, radius near the safety floor */
    TD5_TG_DUAL_LANE,      /* widened multi-lane stretch (tapered in and out) */
    TD5_TG_SECTION_COUNT
} TD5_TrackGenSection;

typedef struct {
    unsigned int seed;              /* 0 = roll one from the clock */
    int  target_spans;              /* total road spans to emit */
    int  lanes;                     /* base lane count (1..12) */
    int  lane_width;                /* world units per lane */
    int  span_length;               /* world units between spans */
    int  weight[TD5_TG_SECTION_COUNT];  /* relative section mix */
    int  elevation_amplitude;       /* world units; 0 = dead flat */
    int  circuit;                   /* 0 = point-to-point (Phase 1 default) */
    /* Ride-smoothness levers, held as scaled ints so they can be driven from
     * the integer env-knob helpers. Measured against a shipped track: Moscow
     * runs p95 |roll rate| 288 / |pitch rate| 512; raising curve_safety eases
     * the tightest corner, lowering max_grade eases the crests. */
    int  curve_safety_x100;         /* min turn radius / half-width, x100 */
    int  max_grade_x1000;           /* steepest |dY/d(arc)|, x1000 */
} TD5_TrackGenSpec;

/* Module lifecycle (registered in g_td5re_modules, after "trackreg"). Init
 * builds a first track so the selector entry exists from the main menu on. */
int  td5_trackgen_init(void);
void td5_trackgen_shutdown(void);

/* Fill spec with the shipped defaults (seed 0, balanced section mix). */
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec);

/* Apply the [AutoTrack] INI section / TD5RE_AUTOTRACK_* env knobs on top of a
 * spec that has already been defaulted. */
void td5_trackgen_apply_config(TD5_TrackGenSpec *spec);

/* Synthesise a track and write its level entries into
 * re/assets/levels/level<level_num>/. Returns 1 on success, 0 on failure
 * (nothing is left half-written on failure -- the caller should fall back to a
 * real track). On success *out_spans receives the emitted span count.
 */
int td5_trackgen_build_level(const TD5_TrackGenSpec *spec, int level_num,
                             int *out_spans);

/* Reserved identity of the auto-generated track. */
int td5_trackgen_level_number(void);   /* the levelNNN it builds into */
int td5_trackgen_slot(void);           /* its frontend schedule slot */

/* Is this frontend schedule slot the auto-generated track? */
int td5_trackgen_is_auto_slot(int slot);

/* Regenerate the auto track with a fresh seed and (re)register it so the
 * frontend + asset loader can see it. Called once at boot (so the selector
 * entry exists) and again at every race launch that selected it (so each race
 * gets new geometry). Returns 1 on success. */
int td5_trackgen_regenerate(unsigned int seed);

/* Seed actually used by the last successful regenerate (0 if none yet) --
 * surfaced in the HUD/log so a good random track can be reproduced. */
unsigned int td5_trackgen_last_seed(void);

#endif /* TD5_TRACKGEN_H */
