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

/* [PICK] Human name for an auto-track texture page id (e.g. "GUARDRAIL",
 * "FLORA", "WALL_TOWER"), or NULL for reserved/unnamed slots. Valid only for
 * the auto track (page ids are per-track). Used by the dev geometry picker. */
const char *td5_trackgen_page_name(int page);

/* [R19 TREELINE PNG] Loader opt-in for the native-resolution tree-line PNG
 * override. Returns 1 only when TD5RE_AUTOTRACK_TREELINE_PNG is on, `level_number`
 * is the auto-track level, and `page` is one of the tree-line pages the generator
 * emits a loose PNG for. The asset loader (tpage_decode_one) uses this to widen
 * the TD6-only PNG path to the auto-track's tree-line pages without touching
 * shipped tracks, TD6, or any other page. Default OFF (knob unset -> 0). */
int td5_trackgen_treeline_png_page(int level_number, int page);

/* [PICK] Emitter-kind name for an auto-track mesh at (entry, slot), e.g.
 * "flora", "building", "guardrail" -- read from the level's MESHTAG.BIN sidecar
 * (written next to MODELS.DAT; MODELS.DAT itself is unchanged). NULL if the
 * sidecar is absent (older cached build) or the slot has no recorded kind.
 * Lazily (re)loads per seed. Auto track only; dev builds only (NULL in RELEASE). */
const char *td5_trackgen_mesh_kind_name(int entry, int slot);

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

/* --- Streamed scenery: producer side ---------------------------------------
 * A streamed build does the geometry (~300 ms) plus TEXTURES.DAT, writes NO
 * MODELS.DAT, and parks what the scenery phases need so the worker in
 * td5_trackgen_stream.c can run them after the level has loaded. That is what
 * lets a generated track start racing on geometry alone and decorate itself as
 * the player drives; scenery is 99.4 percent of a build and the simulation
 * never reads it.
 *
 * SINGLE INSTANCE: between the streamed build and _discard, nothing else may
 * run a build or a route preview -- the generator keeps ~102 mutable statics
 * and the worker reads the stashed node list. Join both workers first. */
int  td5_trackgen_regenerate_streamed(unsigned int seed);
int  td5_trackgen_stream_pending(void);        /* geometry done, scenery owed */
int  td5_trackgen_stream_entry_count(void);
int  td5_trackgen_stream_span_count(void);
/* Worker-thread only. Publishes each entry via td5_track_scenery_publish_entry
 * as it is assembled; polls *cancel between entries. Returns 1 only if the
 * whole table was published. */
int  td5_trackgen_stream_scenery(volatile int *cancel);
void td5_trackgen_stream_discard(void);        /* frees the stashed node list */

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

/* [R14 GENPERF 2026-09-03] Race-entry build. restart=1 keeps the last seed (a
 * pause-menu RESTART of the same race); otherwise a fresh seed unless
 * TD5RE_AUTOTRACK_SEED pins one. An identical build already on disk (same
 * seed, spec, TD5RE_* knobs and exe) is REUSED without regenerating
 * (TD5RE_AUTOTRACK_REUSE=0 disables). Safe to call from a worker thread while
 * the main thread only draws the loading screen (see td5_game.c). */
/* `streamed` = build GEOMETRY ONLY and park the scenery for the worker (see the
 * streamed-producer section above). Passed in rather than decided here: the knob
 * lives in the consumer module, td5_trackgen_stream.c. */
int td5_trackgen_prepare_race(int restart, int streamed);

/* Build progress 0..100 for the loading-screen bar (readable from any thread). */
int td5_trackgen_progress(void);

/* Is the auto-generated track a NIGHT track? Decided ONCE per race entry (in
 * td5_trackgen_regenerate, which every race launch calls) and latched, so every
 * emitter and every renderer that asks during a build gets the same answer --
 * see the TIME OF DAY block in td5_trackgen.c for why a per-call predicate is
 * wrong. Safe to call at any time; 0 (day) before the first regenerate.
 *
 * Knob: TD5RE_AUTOTRACK_NIGHT (0 = always day, 1 = always night, 2 = decide
 * from the seed, default 2). */
int td5_trackgen_is_night(void);

/* ============ [R21 ROLLS] SEED-DERIVED RANDOMIZED PARAMETERS ===============
 *
 * WHY: every AUTO TRACK STUDIO row used to be a fixed value applied uniformly
 * to the whole track, so two tracks at the same settings differed only in
 * route. A row's default is now RANDOM: the value is derived from the SEED, so
 * a seed still reproduces its track exactly while an unattended studio
 * produces real variety. This generalises what TIME OF DAY has always done
 * (see td5_trackgen_is_night above) to every parameter.
 *
 * RANDOM IS AN EXPLICIT VALUE, NOT THE ABSENCE OF ONE. The knob's own spelling
 * of RANDOM is "unset" (that is what keeps tg_env_hash, the favourites store
 * and every td5_env_* default consistent), but the studio's value tables carry
 * TD5_TG_ROLL_RANDOM as a first-class member so a row can display it, cycle
 * back to it, and reverse-map it like any other choice.
 *
 * -2 rather than -1: k_at_sky_v[0] is already -1 ("SKY = NONE"), so -1 is a
 * live value in the studio's tables and cannot double as a sentinel.
 *
 * PURITY RULE, load-bearing for the on-disk build cache: a roll may depend on
 * NOTHING but the seed and the TD5RE_* environment. Both already feed the
 * GENSTAMP (seed directly, knobs via tg_env_hash), so a seed-derived roll needs
 * no stamp change -- same seed, same roll, same MODELS.DAT, and REUSE stays
 * correct. A roll that consulted the clock, a frame counter or previous build
 * state would silently serve a stale track. */
#define TD5_TG_ROLL_RANDOM (-2)

typedef enum {
    TD5_TG_ROLL_TWIST = 0,   /* -> weight[STRAIGHT/CURVE/ACUTE] (composite)   */
    TD5_TG_ROLL_CORNERS,     /* -> curve_safety_x100                          */
    TD5_TG_ROLL_GRADE,       /* -> max_grade_x1000                            */
    TD5_TG_ROLL_DUAL,        /* -> weight[DUAL_LANE]                          */
    TD5_TG_ROLL_HILLS,       /* -> elevation_amplitude                        */
    TD5_TG_ROLL_NIGHT,       /* delegates to tg_decide_night, reported here    */
    TD5_TG_ROLL_COUNT
} TD5_TgRollId;

/* Resolved table for one build. Rolls are keyed by an entry's private SALT and
 * never by its position, so appending an id here can never move an existing
 * entry's roll -- and therefore never changes an existing seed's track. */
typedef struct {
    unsigned int  seed;
    int           value [TD5_TG_ROLL_COUNT];  /* resolved literal (180, 6000) */
    unsigned char choice[TD5_TG_ROLL_COUNT];  /* index into the choice set     */
    unsigned char pinned[TD5_TG_ROLL_COUNT];  /* 1 = the knob pinned it        */
} TD5_TgRolls;

/* PURE: touches no statics, consumes no RNG, safe on any thread. Same
 * (seed, environment) gives the same result as the build will use, which is how
 * the studio can label a row "RANDOM (TWISTY)" without waiting for a preview. */
void td5_trackgen_resolve_rolls(unsigned int seed, TD5_TgRolls *out);

int         td5_trackgen_roll_choice_count(int id);
const char *td5_trackgen_roll_choice_name(int id, int choice);
const char *td5_trackgen_roll_name(int id);
/* Straight/curve/acute weights for a TWISTINESS choice. The mix lives with the
 * generator, not the frontend, so the studio and the walk cannot disagree. */
int         td5_trackgen_twist_mix(int choice, int out3[3]);

#endif /* TD5_TRACKGEN_H */
