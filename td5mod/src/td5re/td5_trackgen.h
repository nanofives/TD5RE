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

/* ---------------------------------------------------------------- preview --
 * Mesh-free 2D route preview (PORT-ONLY). Runs only the PURE half of a build
 * -- biome layout, centerline walk, elevation profile, strip emit -- and hands
 * the caller the route as it is walked. It emits no scenery, bakes no texture
 * pages and writes no file, so it costs a fraction of a real build.
 *
 * The strip emit is included on purpose. Branch corridors are NOT part of the
 * centerline: tg_emit_strip is what fills s_forks[] and s_ring_len, so without
 * it a preview would draw the main ring, silently omit every branch, and not
 * know where the finish line falls.
 *
 * NOT re-entrant, and NOT safe to run concurrently with a real build -- both
 * walk the same module statics (the private RNG, the biome grid). The caller
 * owns mutual exclusion. td5_trackgen_preview.c provides it: one worker thread,
 * which td5_asset_load_level joins before it calls td5_trackgen_regenerate.
 */
typedef struct {
    float x, z;      /* raw world units; the caller normalises for display */
    int   lanes;
    int   branch;    /* 0 = main ring, 1..N = branch corridor index */
} TD5_TrackGenPoint;

typedef struct {
    /* Newly walked points, in order. Called many times per build. */
    void (*on_points)(const TD5_TrackGenPoint *pts, int n, void *ctx);
    /* Polled per node; returning non-zero aborts the walk promptly. */
    int  (*should_cancel)(void *ctx);
    void *ctx;
} TD5_TrackGenPreviewSink;

typedef struct {
    unsigned int seed;
    int node_count;                      /* centerline nodes walked */
    int span_count;                      /* strip spans incl. branch corridors */
    int ring_len;                        /* main-ring spans; finish lives here */
    int fork_count;
    int tally[TD5_TG_SECTION_COUNT];     /* sections actually placed */
    int min_y, max_y;                    /* elevation range, world units */
    int cancelled;                       /* 1 = aborted via should_cancel */
} TD5_TrackGenPreviewStats;

/* Returns 1 when a full route was produced, 0 on failure OR cancellation
 * (check out_stats->cancelled to tell them apart). `sink` may be NULL, in
 * which case this is just a silent dry-run that fills out_stats. */
int td5_trackgen_preview_route(const TD5_TrackGenSpec *spec,
                               const TD5_TrackGenPreviewSink *sink,
                               TD5_TrackGenPreviewStats *out_stats);

/* Module lifecycle (registered in g_td5re_modules, after "trackreg"). Init
 * builds a first track so the selector entry exists from the main menu on. */
int  td5_trackgen_init(void);
void td5_trackgen_shutdown(void);

/* Fill spec with the shipped defaults (seed 0, balanced section mix). */
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec);

/* Apply the TD5RE_AUTOTRACK_* env knobs on top of a spec that has already been
 * defaulted. Env ONLY -- there is no [AutoTrack] INI section, and the AUTO
 * TRACK options screen deliberately keeps these per-session (it writes them
 * with _putenv_s; see the header comment on k_at_rows in td5_fe_race.c). */
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

/* As above but GEOMETRY ONLY: strip, routes, levelinf and sky, with no
 * MODELS.DAT and no texture pages. Used by td5_trackgen_init, because the boot
 * build exists only to register the selector entry and every race launch
 * regenerates from scratch -- so boot scenery is written and then thrown away
 * unread. Measured at 99.4 percent of boot cost. The registry entry is
 * identical either way: the finish span comes from the strip, not the scenery.
 *
 * Independent of TD5RE_AUTOTRACK_SCENERY, which stays the player's choice. */
int td5_trackgen_regenerate_geometry_only(unsigned int seed);

/* [S2 / Phase 2 streaming] Rebuild the main-road span records for `seed` and
 * return them as a blob of 24-byte records for the caller to free. Lets the
 * track module overwrite a region of its LIVE span array with bytes that
 * provably match what the seed produced originally. Never touches live state;
 * deterministic across calls. See docs/plans/AUTOTRACK_STREAMING.md. */
int td5_trackgen_regenerate_main_spans(unsigned int seed,
                                      unsigned char **out_bytes,
                                      int *out_span_count);

/* Seed actually used by the last successful regenerate (0 if none yet) --
 * surfaced in the HUD/log so a good random track can be reproduced. */
unsigned int td5_trackgen_last_seed(void);

/* Is the auto-generated track a NIGHT track? Decided ONCE per race entry (in
 * td5_trackgen_regenerate, which every race launch calls) and latched, so every
 * emitter and every renderer that asks during a build gets the same answer --
 * see the TIME OF DAY block in td5_trackgen.c for why a per-call predicate is
 * wrong. Safe to call at any time; 0 (day) before the first regenerate.
 *
 * Knob: TD5RE_AUTOTRACK_NIGHT (0 = always day, 1 = always night, 2 = decide
 * from the seed, default 2). */
int td5_trackgen_is_night(void);

#endif /* TD5_TRACKGEN_H */
