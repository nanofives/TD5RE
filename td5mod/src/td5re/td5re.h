/**
 * td5re.h -- Master header for TD5RE source port
 *
 * Provides:
 *  - Forward declarations for all modules
 *  - Global game state struct
 *  - Module init/shutdown/tick function pointer table
 *  - Master init/shutdown entry points
 */

#ifndef TD5RE_H
#define TD5RE_H

#include "td5_types.h"

/* ========================================================================
 * Module Forward Declarations
 * ======================================================================== */

/* Each module exposes init/shutdown/tick functions */

/* td5_game.c -- Main loop, state machine */
int  td5_game_init(void);
void td5_game_shutdown(void);
int  td5_game_tick(void);

/* td5_physics.c -- Vehicle dynamics */
int  td5_physics_init(void);
void td5_physics_shutdown(void);
void td5_physics_tick(void);

/* td5_track.c -- Track geometry, segment contacts */
int  td5_track_init(void);
void td5_track_shutdown(void);

/* td5_ai.c -- AI routing, rubber-banding */
int  td5_ai_init(void);
void td5_ai_shutdown(void);
void td5_ai_tick(void);

/* td5_render.c -- Scene setup, rendering */
int  td5_render_init(void);
void td5_render_shutdown(void);
void td5_render_frame(void);

/* td5_frontend.c -- Menu screens */
int  td5_frontend_init(void);
void td5_frontend_shutdown(void);
void td5_frontend_tick(void);

/* td5_hud.c -- Race HUD, minimap */
int  td5_hud_init(void);
void td5_hud_shutdown(void);
void td5_hud_render(void);

/* td5_sound.c -- Audio */
int  td5_sound_init(void);
void td5_sound_shutdown(void);
void td5_sound_tick(void);

/* td5_input.c -- Input polling */
int  td5_input_init(void);
void td5_input_shutdown(void);
void td5_input_tick(void);

/* td5_asset.c -- ZIP loading, texture upload */
int  td5_asset_init(void);
void td5_asset_shutdown(void);

/* td5_save.c -- Config/cup save/load */
int  td5_save_init(void);
void td5_save_shutdown(void);

/* td5_net.c -- Multiplayer */
int  td5_net_init(void);
void td5_net_shutdown(void);
void td5_net_tick(void);

/* td5_camera.c -- Camera system */
int  td5_camera_init(void);
void td5_camera_shutdown(void);
void td5_camera_tick(void);

/* td5_vfx.c -- Particles, effects */
int  td5_vfx_init(void);
void td5_vfx_shutdown(void);
void td5_vfx_tick(void);

/* td5_fmv.c -- Video playback */
int  td5_fmv_init(void);
void td5_fmv_shutdown(void);
int  td5_fmv_play(const char *filename);

/* ========================================================================
 * Module Table
 *
 * Allows iterating all modules for batch init/shutdown.
 * ======================================================================== */

typedef struct TD5_Module {
    const char *name;
    int  (*init)(void);
    void (*shutdown)(void);
} TD5_Module;

/** Array of all modules in initialization order. */
extern const TD5_Module g_td5re_modules[];
extern const int        g_td5re_module_count;

/* ========================================================================
 * Global Game State
 *
 * Central state structure that modules read/write through td5_game.
 * ======================================================================== */

/* [dynamic-traffic 2026-06] Number of traffic-volume tiers in the selector and
 * in g_td5.traffic_volume: 0=OFF 1=LOW 2=MEDIUM 3=HIGH 4=VERY HIGH. The frontend
 * cycles 0..(COUNT-1) and clamps committed values to this range; the AI spawner
 * (td5_ai.c trf_dyn_*) consumes the resolved 0..4 value. */
#define TD5_TRAFFIC_VOLUME_COUNT 5

typedef struct TD5_GlobalState {
    /* State machine */
    TD5_GameState   game_state;
    TD5_GameType    game_type;
    TD5_Difficulty  difficulty;
    /* AI-car tier row (0..2) — mirrors original's gRaceDifficultyTier
     * @ 0x00463210. Set from game_type in ConfigureGameTypeFlags per the
     * original's switch at 0x00410CA0. Indexes s_difficulty_tier_cars[].
     * Default = 2 (matches original's runtime boot state on single-race
     * path — [CONFIRMED via Frida 2026-04-20]). */
    int             difficulty_tier;
    TD5_WeatherType weather;

    /* Flags */
    int  quit_requested;
    int  race_requested;
    int  race_confirmed;
    int  frontend_init_pending;
    int  intro_movie_pending;
    int  benchmark_active;
    int  paused;
    int  network_active;

    /* Race configuration */
    int  time_trial_enabled;
    /* Solo-synth mode: user-facing "Time Trial" with the engine running as
     * plain single race (game_type=0) plus slots 1..5 forced INACTIVE. Mirrors
     * the Frida TT-synth applied to the original via td5_quickrace_hook.js so
     * /diff-race captures apples-to-apples physics on slot 0. Set by
     * ConfigureGameTypeFlags case 7. */
    int  solo_mode_synth;
    int  wanted_mode_enabled;
    int  drag_race_enabled;
    int  traffic_enabled;
    /* [PORT ENHANCEMENT dynamic-traffic] User-facing traffic VOLUME committed
     * from the track-select / game-options row: 0=Off 1=Low 2=Medium 3=High.
     * traffic_enabled stays the boolean gate every faithful site already reads
     * (== volume != 0). Consumed by the dynamic spawner's concurrency cap
     * (Low=2 / Medium=4 / High=6 cars); with [Traffic] Dynamic=0 any
     * non-zero volume behaves like the classic ON (all 6 queue slots).
     * [dynamic-traffic 2026-06] Extended to a 5-state row: 4=Very High (see
     * TD5_TRAFFIC_VOLUME_COUNT). The AI spawner (td5_ai.c trf_dyn_*) consumes
     * value 4 directly; the frontend selector now emits the full 0..4 range. */
    int  traffic_volume;
    int  special_encounter_enabled;
    int  circuit_lap_count;
    int  checkpoint_timers_enabled;
    int  race_rule_variant;
    int  reverse_direction;
    int  dynamics_mode;  /* 0=arcade, 1=simulation */

    /* [MP GAME MODES 2026-06-22] Live multiplayer mode + per-mode options,
     * edited on the mode-vote/mode-config screens and applied at race init.
     * In a net race this mirrors the host's choice (replicated via DXPSTART);
     * in single-player it stays TD5_MP_MODE_RACE. See TD5_MpModeConfig. */
    TD5_MpModeConfig mp_mode_config;

    /* Quick Race player setup (infra to later replace the Two-Player menu).
     * num_human_players + num_ai_opponents <= TD5_MAX_RACER_SLOTS (16).
     * [PORT ENHANCEMENT 2026-06] N-way split-screen: up to TD5_MAX_HUMAN_PLAYERS
     * (9) local humans, each with its own viewport (see td5_game viewport ladder).
     * The original was hard-capped at 2 viewports / 6 racers; this path deviates
     * freely. Defaults (1 human + 5 AI = 6) reproduce the legacy single-race grid. */
    int  num_human_players;   /* 1..TD5_MAX_HUMAN_PLAYERS (9) */
    int  num_ai_opponents;    /* 0..TD5_MAX_RACER_SLOTS-1 (15) */

    /* [MP AI TEST PLAYERS 2026-06-25] Dev-only "add AI player" lobby tool: bit s
     * set => local split-screen player slot s is a SIMULATED player — it occupies
     * a human-pool slot (so it gets its own viewport/HUD/results row) but is driven
     * by the race AI, not a controller. Lets one person stress every multiplayer
     * code path (split-screen, cop roles, results table) without N humans/pads.
     * Computed in frontend_init_race_schedule from s_mp_slot_is_ai[]; consumed in
     * InitRace to flip those slots to AI (state=0). 0 = no AI test players. */
    uint32_t mp_ai_player_mask;

    /* Viewport */
    int  render_width;
    int  render_height;
    int  split_screen_mode;     /* 0=single, 1=horiz, 2=vert */
    int  viewport_count;
    /* [PORT ENHANCEMENT 2026-06] Multiplayer Options split-layout picker.
     * The chosen grid for N-way split: split_grid_cols x split_grid_rows. The N
     * human players fill the first N cells (row-major); cells N..cols*rows-1 are
     * "missing" and carry a (deferred) content selector. When cols/rows are 0
     * the viewport layout falls back to the automatic ladder (harness path).
     * split_missing_content[k] = stub content id for the k-th empty cell. */
    int  split_grid_cols;
    int  split_grid_rows;
    int  split_missing_content[2];
    /* [PORT ENHANCEMENT 2026-06-08] AI spectator split-screens (dev/profiling).
     * Number of AI-driven cars (slots 1..N) each rendered in its own extra
     * viewport pane, on top of the human pane(s). viewport_count becomes
     * num_human_players + num_spectate_screens. Input still goes only to the
     * humans; the spectator panes just follow AI cars. Set by the Quick Race
     * "AI Screens" selector (interactive) or the [Game] SpectateScreens knob
     * (AutoRace harness). 0 = off = legacy. Dev-only (release clamps to 0). */
    int  num_spectate_screens;

    /* Timing */
    float sim_tick_budget;
    uint32_t sim_time_accumulator;
    int   simulation_tick_counter;
    float normalized_frame_dt;
    float instant_fps;
    uint32_t frame_prev_timestamp;
    uint32_t frame_end_timestamp;

    /* Gravity */
    int32_t gravity_constant;

    /* Track info */
    int32_t track_span_ring_length;
    int32_t track_start_span_index;   /* g_trackStartSpanIndex — circuit start/finish span */
    int     track_type;         /* TD5_TrackType */
    int     track_index;
    int     car_index;
    int     ai_car_indices[TD5_MAX_RACER_SLOTS];  /* per-slot car index for AI racers (slot 1..N-1) */
    int     ai_car_variants[TD5_MAX_RACER_SLOTS]; /* per-slot color variant (0..3) */

    /* Frontend */
    int  frontend_screen_index;
    int  frontend_inner_state;
    int  frontend_frame_counter;

    /* Race state */
    int  race_end_fade_state;
    int  fade_direction;
    int  total_actor_count;

    /* INI defaults (loaded from td5re.ini by main.c) */
    struct {
        /* Display */
        int  fog_enabled;
        int  speed_units;
        int  camera_damping;
        /* Enumerated display-mode ordinal (DisplayOptions resolution row).
         * Formerly persisted in Config.td5 (+0xBD); now a [Display] DisplayMode
         * key in td5re.ini. The applied resolution still comes from Width/Height. */
        int  display_mode;
        /* [S01 Display options 2026-06-04] new display knobs:
         *   window_mode : 0=fullscreen exclusive, 1=windowed, 2=borderless ([Display] WindowMode)
         *   vsync       : 1=wait for vblank, 0=uncapped/tearing ([Display] VSync)
         *   show_fps    : 1=show the FPS/MS overlay, 0=hide it ([Display] ShowFps)
         *   disp_width / disp_height : the chosen windowed/fullscreen resolution,
         *     persisted to [Display] Width/Height so it survives a relaunch. */
        int  window_mode;
        int  vsync;
        int  show_fps;
        int  disp_width;
        int  disp_height;
        /* Audio */
        int  sfx_volume;
        int  music_volume;
        int  sfx_mode;
        int  radio_enabled;     /* 1 = use the internet-radio music backend */
        int  radio_volume;      /* radio output volume 0..100 (own knob, low default) */
        char radio_url[512];    /* radio stream URL (Icecast/SHOUTcast MP3/AAC) */
        /* Lighting (dynamic light system foundation, 2026-06-30). See
         * td5_light.c / td5_render.c. The original engine has no point lights or
         * headlights — this is a port extension on the per-vertex lighting seam. */
        int  lighting_enabled;  /* 1 = dynamic light system on (default) */
        int  headlights;        /* 1 = vehicle headlights (manual toggle, used when Auto=0) */
        int  light_dark_mode;   /* 1 = dim scene so headlights illuminate dark areas (default 0) */
        int  lighting_auto;     /* 1 = auto-enable headlights in poorly-lit environments (default) */
        /* Game options */
        int  laps;
        int  checkpoint_timers;
        int  traffic;
        int  cops;
        int  difficulty;
        int  dynamics;
        int  collisions;
        int  powerups;   /* 1 = ARCADE power-up item boxes enabled (default), 0 = off */
        /* AutoGearbox: 1 = automatic transmission (default; gear up/down keys
         * ignored), 0 = manual. Drives input bit 28 → actor+0x378 which orig
         * UpdatePlayerVehicleControlState @ 0x00402E60 gates the gear-shift
         * block on. The orig defaults to auto; in port the menu toggle is
         * not wired, so this INI key is the only way to switch to manual. */
        int  auto_gearbox;
        /* LaneAssist (port-only accessibility aid, 2026-06-28): per-session
         * default enable (0 = off, the default; 1 = on) for the optional
         * lane-assist steering aid — a gentle, capped nudge toward the nearest
         * lane-centre line, smooth across forks/merges. Per-player at runtime
         * (keyboard 'L' toggles player 0). Strength/look-ahead/cap/fork-commit
         * are env knobs (TD5RE_LANEASSIST_*). See td5_laneassist.c. */
        int  lane_assist;
        /* [DRAG RACE OPTIONS 2026-06-29] Drag-race mode menu options.
         * drag_length: 0=SHORT 1=MEDIUM 2=LONG 3=EPIC (EPIC repeats the strip).
         * drag_traffic: 0=off, 1=oncoming traffic. */
        int  drag_length;
        int  drag_traffic;
        /* RearImpactResponse (port-only playability knob, S08 2026-06-04):
         * percentage of the original car-vs-car ANGULAR response (yaw spin +
         * heavy-impact scatter/lift) to retain when a HUMAN player is struck on
         * its REAR face. 100 = byte-faithful (no softening), lower = gentler.
         * Front/side contacts, the attacker's response, and all AI/traffic
         * collisions stay fully faithful. Default 45 tames getting rear-ended
         * from an uncontrollable spin-out into a recoverable nudge; the linear
         * shove is left faithful so the hit still reads as a real bump.
         * Clamped to [0,100] at parse. */
        int  rear_impact_response;
        /* AntiTunnel (port-only anti-interpenetration knob, S17 2026-06-05):
         * 1 (default) = run the position-only car-vs-car depenetration
         * relaxation pass after the faithful impulse resolution, so a
         * high-speed pair cannot visibly pass THROUGH each other for a frame.
         * 0 = byte-faithful (original 0x004079C0 single fixed-step push only,
         * which can leave a deep/fast overlap interpenetrated for >=1 frame).
         * The pass moves ONLY world position — velocities and yaw spin are
         * never touched, so the S08 RearImpactResponse / impact tuning is
         * unaffected. Clamped to {0,1} at parse. */
        int  anti_tunnel;
        /* AntiTunnelSlop (port-only, S17 2026-06-05): how many display units of
         * collision-OBB overlap the depenetration pass is allowed to LEAVE
         * before pushing cars apart. The original collision box is slightly
         * larger than the visible car mesh, so depenetrating to zero OBB
         * overlap parks cars with a visible gap (~combined box-vs-mesh margin).
         * Allowing this much OBB overlap lets cars rest at ~visible-mesh
         * contact. ~3 display units ~= 1 cm. Only the resting position changes;
         * the faithful impulse/bounce timing is untouched. Clamped [0,256]. */
        int  anti_tunnel_slop;
        /* PhysicsLOD (S30 2026-06-06, default 1): distant-car physics level-of-
         * detail for LARGE fields (>8 racers). When on, racers far from the
         * camera car AND off-screen run the expensive per-car track-contact work
         * (span-walk, ground-snap, suspension, wall/edge resolvers) every 2nd tick
         * instead of every tick — on the skipped tick they "coast" on their last
         * velocity. Keeps AI steering + motion every tick so cars stay on-route;
         * only the invisible-at-distance detail is rate-limited. Faithful 6-car
         * races are untouched (gate requires >8 racers). 0 = off (full physics). */
        int  physics_lod;
        /* CatchupAssist (S06 2026-06-04): td5re.ini override for the persisted
         * CATCHUP / rubber-band assist level. -1 (default) = use the persisted
         * value (S05 toggle, default 1 = on/softened). 0 = catchup off; 1..9 =
         * on. When >= 0 this wins over the persisted value (a test/power-user
         * override). Resolved by ai_catchup_level() in td5_ai.c. */
        int  catchup_assist;
        /* AIAccelFromCar (S06 2026-06-04, default 1): when 1, AI racers' and
         * traffic vehicles' acceleration (drive-torque +0x68) and top speed
         * (+0x74) are sourced from each car's OWN carparam.dat, scaled by a
         * per-difficulty-tier factor, instead of the difficulty-only template
         * constants. Bicycle-critical fields (Wf/Wr/I/grip) stay on the AI
         * template for stability. 0 = faithful template-only behaviour. */
        int  ai_accel_from_car;
        /* --- Smart Opponent AI (source-port overhaul, NON-FAITHFUL) ---------
         * A from-scratch decision brain for the racing opponents (and, when
         * scope includes it, background traffic). Replaces the original's
         * centreline-follow + slot-parity-branch + nearest-peer-nudge with:
         *   - lane selection scored by surface / occupancy / wall proximity,
         *   - strategic branch choice at junctions,
         *   - car-following speed control (ease/brake for a blocked lane),
         *   - a continuous per-car skill that scales competence with difficulty,
         *   - a gentle, symmetric rubber-band "leash".
         * The tuned steering cascade + physics interface are left untouched —
         * only the DECISIONS (lateral target, branch, throttle) change. When
         * smart_ai = 0 the AI is byte-faithful to the original. [GameOptions].
         *
         *   smart_ai            : master gate (default 1 = on).
         *   smart_ai_aggression : racecraft when lanes run out —
         *                         0 = clean/defensive (always yield, no contact),
         *                         1 = racing-realistic (hold line, defend, light
         *                             contact) [default],
         *                         2 = aggressive (block + lean on rivals).
         *   smart_ai_leash      : gentle symmetric catch-up leash strength,
         *                         0 = none .. 9 = strong (default 3). Independent
         *                         of the faithful CatchupAssist; only active when
         *                         smart_ai = 1.
         *   smart_ai_rays       : ray-sensing decision brain (default 1 = on).
         *                         When on (and smart_ai = 1) the opponents/traffic
         *                         sense walls + cars with a forward ray fan and
         *                         take a curvature-aware outside-in-outside racing
         *                         line with slow-in/fast-out corner braking.
         *                         When 0, SmartAI falls back to the older
         *                         discrete-lane scorer + hard wall-margin clamp.
         *                         Three-level ladder for A/B:
         *                           smart_ai=0           -> byte-faithful AI
         *                           smart_ai=1, rays=0    -> discrete-lane SmartAI
         *                           smart_ai=1, rays=1    -> ray brain (default). */
        int  smart_ai;
        int  smart_ai_aggression;
        int  smart_ai_leash;
        int  smart_ai_rays;
        /* --- S20 Smart Traffic (source-port enhancement, all default ON) ---
         * The original background traffic is scripted/reactive (flat 0x3c cruise,
         * deterministic junction route, no active wall avoidance). These knobs
         * layer three tunable behaviours on top, applied ONLY to traffic slots
         * (>= g_traffic_slot_base); racing AI (slots 0-5) is untouched. Each can
         * be disabled independently; traffic_smart=0 disables all three (fully
         * faithful traffic). All three operate only on the traffic car's lateral
         * target / sub-lane — they never touch route_state or yaw, so they don't
         * trip the heading-recovery brake or perturb racers. (A 4th behaviour,
         * random branches via route reassignment, was dropped: it froze traffic
         * by desyncing yaw from the route — see td5_ai.c S20 block.) Section
         * [Traffic] in td5re.ini. */
        int  traffic_smart;            /* master gate (default 1) */
        int  traffic_wall_avoid;       /* bias edge-lane target toward lane centre (default 1) */
        int  traffic_avoid_slow_lane;  /* prefer asphalt lane over off-road shoulder (default 1) */
        int  traffic_lookahead;        /* lane-change/ease around a car close ahead (default 1) */
        int  traffic_wall_avoid_bias;  /* edge-lane inward blend, 0..256 (default 96 ~= 0.375) */
        /* AntiFreeze (source-port enhancement, default ON, independent of the
         * three "smart" behaviours above): the faithful traffic recovery-brake
         * only clears when the player drives forward enough to recycle the car,
         * so a heading-misaligned traffic car FREEZES forever when the player is
         * parked/slow (RE-confirmed: RecycleTrafficActorFromQueue @ 0x4353b0
         * gate is player-relative; Stage-2 recovery only cleared by recycle).
         * AntiFreeze un-sticks a car that has been recovery-frozen for
         * `traffic_antifreeze_frames` ticks by clearing the recovery flag and
         * re-aligning its heading to the road, so traffic stays alive even when
         * watched from a standstill. Clearly non-faithful; [Traffic] AntiFreeze. */
        int  traffic_antifreeze;        /* 1 = un-stick recovery-frozen traffic (default 1) */
        int  traffic_antifreeze_frames; /* frozen-tick threshold before un-stick (default 120) */
        /* [PORT ENHANCEMENT dynamic-traffic 2026-06] GTA-style ambient traffic.
         * Replaces the TRAFFIC.BUS fixed-spawn queue with distance-driven
         * spawn/despawn: cars spawn periodically on a random non-slow lane a
         * random span-distance ahead of a (random) local player, fade in over
         * FadeTicks, and fade out + park once EVERY local player is farther
         * than DespawnDistance spans away (multiplayer-aware, unlike the
         * original slot-0-only recycle @ 0x004353B0). Dynamic=0 restores the
         * byte-faithful queue spawn/recycle path untouched. All distances are
         * in track SPANS (the original's own recycle metric, cf. 0x28 spans
         * @ 0x004353EB); section [Traffic] in td5re.ini. */
        int  traffic_dynamic;          /* master gate (default 1) */
        int  traffic_dyn_spawn_min;    /* min spawn distance ahead, spans (default 25) */
        int  traffic_dyn_spawn_max;    /* max spawn distance ahead, spans (default 50) */
        int  traffic_dyn_despawn;      /* fade-out distance from EVERY player, spans (default 65) */
        int  traffic_dyn_fade_ticks;   /* fade in/out duration, 30Hz ticks (default 12) */
        int  traffic_dyn_period;       /* base ticks between spawn attempts (default 45) */
        int  traffic_dyn_speed_pct;    /* cruise-speed scale %, dynamic mode only (default 150, 100=faithful 0x3C) */
        int  traffic_dyn_start_offset; /* start-line clearance: no traffic spawns within N spans after the start line (default 200, 0=off) */
        int  traffic_dyn_circuits;     /* 1 = dynamic traffic also on circuit / LEVELINF-no-traffic tracks incl. TD6 conversions (default 1; 0 = faithful no-traffic circuits) */
        /* PlayerCollide (source-port enhancement, default ON): the faithful V2V
         * broadphase buckets actors by track-span (>>2) and only tests pairs
         * within +/-1 bucket (~4 spans). On curves/junctions a traffic car can be
         * physically on top of the player but several spans apart, so the pair is
         * never tested and the player drives THROUGH it. This adds an explicit
         * per-frame player-vs-each-traffic collision test by world proximity that
         * bypasses the span bucket, so you always bump traffic you overlap. */
        int  traffic_player_collide;    /* 1 = explicit player<->traffic collision (default 1) */
        /* Police chase (rewrite 2026-06-19). The cop is a traffic vehicle that
         * starts chasing the first racer that passes it. Master on/off is the
         * existing [GameOptions] Cops toggle; these tune the rewritten behavior.
         * Section [Police] in td5re.ini; each has a --Key=N CLI override. */
        int  cop_ratio;          /* 1 cop per N regular traffic spawns (default 7, min 1) */
        int  cop_catchup_pct;    /* cop target speed = this % of the chased car's speed (default 150 = 1.5x; clamped 100..300) */
        int  cop_min_speed;      /* chased car must exceed this longitudinal speed to trigger a chase (default 0x15638, faithful gate) */
        int  cop_smoke_ticks;    /* how long a broken-down traffic/cop emits smoke + stays parked, 30Hz ticks (default 150 = 5s) */
        /* Per-player joystick selection (0 = keyboard, >=1 = 1-based enumerated
         * joystick index). Overrides the device index persisted in Config.td5.
         * The original supports up to 2 simultaneous joystick devices. */
        int  player1_joystick;
        int  player2_joystick;
        /* Defaults */
        int  default_car;
        int  default_track;
        int  default_game_type;
        int  td6_paint_color;    /* last-selected TD6 paint color (0xRRGGBB); persisted */
        int  td6_paint_color2;   /* secondary TD6 paint color (0xRRGGBB); used by non-SOLID patterns; persisted */
        int  td6_paint_pattern;  /* TD6 paint pattern (TD6_PAT_*); 0 = SOLID; persisted */
        int  default_opponents;   /* AutoRace AI-opponent count override; -1 = full grid (5) */
        int  circuit_minimap;     /* 1 = draw the in-race minimap on circuit tracks too (port enhancement; orig disabled it). 0 = faithful (no minimap on circuits) */
        int  default_players;     /* AutoRace local-human count override (N-way split test); -1 = schedule default */
        int  spectate_screens;    /* AutoRace AI-spectator split-screen count (dev profiling); 0 = off. See g_td5.num_spectate_screens */
        int  threaded_panes;      /* [Render] ThreadedPanes: 1 = build split-screen panes (>2) on worker threads via per-pane CPU command lists. Dev-only (release clamps to 0). */
        /* TD6 track migration (Phase 1): when > 0, td5_asset_level_number()
         * returns this level number directly (bypassing the schedule->pool->zip
         * chain), so the loader resolves re/assets/levels/level<NNN>/ loose files
         * for a converted TD6 track. 0 = disabled = faithful. See
         * re/analysis/td6_track_migration_plan.md and convert_td6_tracks.py. */
        int  override_track_zip;
        /* TD6 track migration: grid start span for an override track (the per-TD5
         * level start-span table is meaningless for a TD6 level). When > 0 it is
         * used as the start/finish grid span. Pick the start/finish straight
         * (widest, straightest section). 0 = auto-clamp to the opening straight. */
        int  override_start_span;
        int  skip_intro;
        /* [GameOptions] TutorialOverlay: controller-tutorial overlay shown at the
         * start of EVERY race (no persistent seen flag any more). Toggled by the
         * Game Options TUTORIAL on/off row. 0 = off, 1 = on every race (default;
         * gamepad players only), 2 = force every race (dev/test, bypasses the
         * gamepad-only gate). See td5_tutorial.c. */
        int  tutorial_overlay;
        int  debug_overlay;
        int  debug_collisions;   /* 1 = draw wireframe of track wall/span geometry */
        /* [S27 2026-06-05] DEV-ONLY headless test hook for the controller-
         * disconnect pause + reconnect modal. Lets an automated run exercise the
         * full pause->modal->resume cycle with no physical unplug.
         *   sim_joy_loss_player   : -1 = off; >=0 = which player slot to simulate
         *                           losing a controller for.
         *   sim_joy_loss_delay_ms : ms after race start before the simulated loss.
         *   sim_joy_loss_hold_ms  : ms to hold the loss before auto-reconnecting.
         * Wall-clock timed (the sim clock freezes while paused). Zeroed/off in the
         * release build (see main.c TD5RE_RELEASE block). */
        int  sim_joy_loss_player;
        int  sim_joy_loss_delay_ms;
        int  sim_joy_loss_hold_ms;
        /* Trace */
        int  race_trace_enabled;
        int  race_trace_slot;
        int  race_trace_max_frames;
        /* [Trace] TrafficEdgePen=1 enables the per-call CSV mirror in
         * td5_physics.c::traffic_edge_pen, paired with the Frida orig
         * probe at tools/_probes/traffic_edge_pen_probe.js. CSV lands at
         * tools/frida_csv/traffic_edge_pen_port.csv. Inert when 0.
         * [TRACE 2026-05-24 traffic-edge-pen-cluster] */
        int  trace_traffic_edge_pen;
        /* [Trace] TerrainCamProbe=1 enables the per-call CSV mirror in
         * td5_camera.c::UpdateChaseCamera for the three terrain probes
         * (forward-right, forward-left, backward) + the AngleFromVector12
         * pitch/roll inputs. Pairs with the Frida orig probe at
         * tools/_probes/terrain_probe_capture.js. CSV lands at
         * tools/frida_csv/terrain_probe_port.csv. Inert when 0.
         * [TRACE 2026-05-25 terrain-pitch-roll-zeroed OVERSIGHT row] */
        int  trace_terrain_cam_probe;
        int  auto_throttle;         /* 1 = force full throttle for slot 0 */
        int  auto_throttle_value;   /* throttle magnitude under AutoThrottle (0=full 0x100) */
        int  auto_throttle_stop_span; /* AutoThrottle brakes to a stop at this span (0=off) */
        float trace_fast_forward;   /* Speed multiplier during trace capture.
                                     * 1.0 = real-time (default), 2.0 = 2x,
                                     * 0.5 = half-speed, N = Nx. Implemented by
                                     * scaling the normalized frame dt before
                                     * it feeds the sim accumulator. Mirrors
                                     * the Frida-side sim budget clamp so the
                                     * diff-race skill runs in seconds instead
                                     * of minutes. <= 0 is treated as 1.0. */
        unsigned int trace_module_mask;  /* bitmask of TD5_TRACE_MOD_* flags */
        unsigned int trace_stage_mask;   /* bitmask of TD5_TRACE_STG_* flags */
        int  race_trace_max_sim_ticks; /* stop the trace after N sim ticks; 0 = no cap.
                                     * Paired with trace_fast_forward so the skill can
                                     * cap the port's workload at the same simulated
                                     * window the Frida side captures, instead of
                                     * burning time on sim ticks we'll never diff. */
        /* [SelfTest] — in-session automated test suite (dev builds only).
         * See td5_selftest.c: frontend screen walk + multi-race matrix +
         * degradation monitor, one report + exit code per session. */
        int  selftest_enabled;    /* 1 = run the suite on boot (--SelfTest=1) */
        int  selftest_suite;      /* 0 = smoke (fast subset), 1 = full matrix */
        int  selftest_race_ticks; /* sim ticks per scripted race (default 450 = 15s @30Hz) */
        int  loaded;  /* 1 once INI has been read */
        /* AutoRace: skip frontend, launch race immediately with INI settings */
        int  auto_race;             /* 1 = auto-start race on launch */
        /* StartScreen: jump directly to a specific frontend screen on boot.
         * -1 = normal flow (STARTUP_INIT → LOCALIZATION → LANGUAGE → LEGAL → MAIN_MENU).
         * 0-29 = jump to that screen index after frontend resources are ready.
         * Ignored when AutoRace=1. Use with SkipIntro=1 to bypass legal screens.
         * Screen index map — mirrors td5_types.h TD5_ScreenIndex:
         *   0=LocalizationInit  1=PositionerDebug  2=AttractMode    3=LanguageSelect
         *   4=LegalCopyright    5=MainMenu         6=RaceTypeMenu   7=QuickRace
         *   8=ConnectionBrowser 9=SessionPicker   10=CreateSession  11=NetworkLobby
         *  12=OptionsHub       13=GameOptions     14=ControlOptions 15=SoundOptions
         *  16=DisplayOptions   17=TwoPlayerOpts   18=ControllerBinding 19=MusicTest
         *  20=CarSelection     21=TrackSelection  22=ExtrasGallery  23=HighScore
         *  24=RaceResults      25=NameEntry       26=CupFailed      27=CupWon
         *  28=StartupInit      29=SessionLocked */
        int  start_screen;         /* -1 = normal, 0-29 = jump to screen */
        /* Shift every actor's spawn span by this many units along the track
         * ring (mirrors the Frida InitializeActorTrackPose hook in
         * re/tools/quickrace/td5_quickrace_hook.js). Read from td5re.ini
         * [Game] StartSpanOffset, overridable via --StartSpanOffset=N on the
         * command line. 0 = vanilla grid. */
        int  start_span_offset;
        /* Force AutoRace track direction to backwards (s_track_direction=1).
         * 0 = forwards (default). 1 = backwards, picks STRIPB.DAT/LEFTB.TRK/
         * RIGHTB.TRK/TRAFFICB.BUS in td5_asset_load_level. Only consumed by
         * td5_frontend_auto_race_setup; the manual frontend Direction toggle
         * is unaffected. Test override only — no equivalent in the original. */
        int  default_reverse;
        /* [CAR DAMAGE 2026-06-28] Global GTA4-style car-damage master. Now ON by
         * default (the player controls it via the Game Options CAR TOUGHNESS +
         * DEFORMATION level rows; there is no in-menu off). [Game] CarDamage=0 in
         * td5re.ini (or --CarDamage=0) is the faithful-sim kill-switch that
         * disables the whole system. Fine-tuned by the TD5RE_DAMAGE_* env knobs. */
        int  car_damage;
        /* [CAR DAMAGE 2026-06-29] Global, persistent damage LEVELS, edited on the
         * Game Options screen and applied to EVERY race. 0=Low, 1=Normal, 2=High.
         * Toughness scales car health (durability); Deform scales the per-vertex
         * dent + scuff magnitude. Persisted to td5re.ini [Game] via
         * td5_ini_persist_options(); --CarToughness/--CarDeform override. */
        int  car_damage_toughness;   /* 0=Low 1=Normal 2=High */
        int  car_damage_deform;      /* 0=Low 1=Normal 2=High */
        /* [CAR DAMAGE 2026-06-29] HUD damage bar + wreck/knockout master. ON by
         * default. When 0, the top-of-pane health bar is hidden AND a car can no
         * longer be wrecked / eliminated by accumulated damage (no race-ending
         * knockout, no health-based handling penalty, no damage smoke). Visual
         * deformation (dents) and collision physics are UNAFFECTED — they are
         * impact-driven, not health-driven. Global, so in split-screen it applies
         * to every player at once (all panes or none). Persisted to td5re.ini
         * [Game] CarDamageBar; --CarDamageBar=N overrides. Edited on the Game
         * Options screen. */
        int  car_damage_bar;
        /* Enable benchmark mode: redirects main-menu button 2 to TD5_GAMESTATE_BENCHMARK.
         * Matches the dead-code path in TD5_d3d.exe gated by app+0x170 (always 0 in
         * the shipped binary). Default 0 = button 2 is 2-player, matching the
         * original's runtime behavior. */
        int  enable_benchmark;
        /* Player-as-AI autopilot (mirrors original attract-mode switch at
         * InitializeRaceSession 0x0042ACCF: slot[0].state = 1 - attract).
         * When set, slot 0 is driven by td5_ai_update_race_actors and the
         * human input-to-actor path is skipped, exactly like demo mode. */
        int  player_is_ai;
        /* OtherPlayersAI (port-only N-way test, 2026-06-03): when set, every
         * LOCAL human slot EXCEPT slot 0 is put on AI autopilot, so player 1
         * (slot 0) is driven by real input while the other split-screen panes
         * drive themselves. Distinct from player_is_ai (which AI-drives slot 0
         * too); the slot-0 AI-dispatch paths key off player_is_ai only, so
         * slot 0 stays human under this knob. */
        int  others_ai;
        /* SoloAISlot (port-only debug, 2026-05-22): which slot drives in
         * solo mode. Default 0. Affects td5_ai_update_track_offset_bias
         * peer-emulation gate and slot-state init. Used to A/B test the
         * solo cascade behavior across all AI personalities. */
        int  solo_ai_slot;
        /* SoloRace (port-only debug, 2026-05-30): when 1, force a 1-racer race
         * (g_racer_count=1) in any game type so the player always finishes 1st.
         * Used to reliably test the victory star/position overlay without having
         * to out-drive the AI. Opponent slots are not spawned, so the position
         * sort ranks slot 0 first (race_position 0). */
        int  solo_race;
        /* MaxSpan (port-only debug, 2026-05-22): if > 0, race auto-exits
         * when slot 0's span_normalized reaches this value. Used to
         * benchmark per-slot AI behavior over the same track distance. */
        int  max_span;
        /* PhantomPeer (port-only debug, 2026-05-22): when 1, solo mode
         * synthesizes a fake peer actor in slot 1 so orig's unmodified
         * find_offset_peer + update_track_offset_bias produce emergent
         * per-slot bias dynamics naturally. When 0, falls back to the v1
         * solo-emulation push cycle. Default 1. */
        int  phantom_peer;
        /* FrontendDraw: when 1, log every fe_draw_quad call to
         * log/frontend_draw_port.csv (screen, page, x, y, w, h, color, uvs).
         * Disabled by default — enable with FrontendDraw=1 in [Logging]. */
        int  log_frontend_draw;
        /* VectorUI: when 1, render frontend BodyText via the resolution-
         * independent MSDF path (crisp at any resolution) instead of the
         * bitmap glyph atlas. Falls back to bitmap if the MSDF atlas/shader
         * fail to load. [Frontend] VectorUI, default 1. */
        int  vector_ui;
        /* Logging — gates for the platform multi-file logger and the D3D
         * ddraw_wrapper log. Defaults preserve current behavior. Flip to
         * compare A/B perf without touching the build. */
        int  log_enabled;     /* master switch (1=on) */
        int  log_min_level;   /* 0=DEBUG 1=INFO 2=WARN 3=ERROR */
        int  log_frontend;    /* frontend.log (frontend, hud, save, input) */
        int  log_race;        /* race.log (game, physics, ai, track, camera, vfx) */
        int  log_engine;      /* engine.log (render, asset, platform, sound, net, fmv) */
        int  log_wrapper;     /* wrapper.log (D3D11 ddraw shim) */
        int  test_resolution_cycle; /* if >0, main loop cycles 3 windowed resolutions to validate apply_display_mode */
        /* Replay persistence — DIVERGENT from original. M2DX DXInput in
         * TD5_d3d.exe never opens a real file (5 methods @ 0x1000A640..0x1000A780
         * have zero file-I/O callees, per reference_replay_td5_is_memory_only.md).
         * Port-only feature: when 1, td5_input_write_close() flushes the
         * input ring to replay.td5 on disk; td5_input_read_open() reads it
         * back. Default 0 preserves faithful in-memory-only behavior. */
        int  replay_persist_to_disk;
        int  test_cup_roundtrip; /* if >0, run td5_save_test_cup_roundtrip() during startup and exit with the result */
        /* Experimental: enable the per-slot pre-loop ClassifyTrackOffsetClamp
         * + bias-rewrite + surface/contact clamp + boundary writeback chain
         * from UpdateRaceActors @ 0x00436A70 inside
         * td5_ai_refresh_route_state_slot. Default 0 = current behaviour
         * (selector + forward_track_component only). 1 = run the missing
         * pre-loop body byte-faithfully against the original. Gated so the
         * change can be A/B-tested per session. */
        int  experimental_bias_clamp;
        /* S10 net-play: [Network] connection config. */
        int  net_mode;          /* 0=LAN auto-discovery (default), 1=Direct IP */
        int  net_game_port;     /* host UDP game port (default 37050) */
        int  net_enable_upnp;   /* Direct host: open the port via UPnP (default 1) */
        char net_nickname[32];  /* player nickname shown in the lobby roster */
        /* [PLAYER NAME 2026-07-02] Game Options PLAYER NAME row: the player's
         * display name for single-player race results and the prefill of the
         * post-race high-score name entry. 15 chars + NUL to match the
         * high-score commit, which copies 15 bytes and forces a NUL at +0xF
         * (ScreenPostRaceNameEntry case 4 @ 0x004140A3-0x004140DD). Empty =
         * unset (results fall back to "P1", name entry to "PLAYER"). */
        char player_name[16];
        /* [Game] CelebrityNamesAPI: when 1, fetches first names from
         * randomuser.me at startup and applies them to NPC leaderboard slots
         * that still hold original default names (no player score set yet).
         * 0 = use the hardcoded celebrity list (default). */
        int  celebrity_names_api;
    } ini;

} TD5_GlobalState;

/** The single global state instance. */
extern TD5_GlobalState g_td5;

/* >0 when the loaded level is a substituted TD6 track (via OverrideTrackZip OR a
 * TD6 menu registry slot). Gates the TD6 engine fixes (seam/lap/grass/AI/render).
 * Set by td5_asset_level_number(); 0 = faithful TD5 track. Standalone global (NOT
 * a g_td5 field) so adding it doesn't change the g_td5 struct layout. */
extern int g_active_td6_level;

/* ========================================================================
 * Per-racer race telemetry (#10 race-end summary rework)
 * ========================================================================
 *
 * One TD5_RaceMetrics per racer slot, accumulated INSIDE the deterministic
 * 30 Hz sim tick from replicated actor state only (no wall-clock / render-rate
 * inputs), so the values are identical on every lockstep netplay client and
 * reproduce exactly on replay.
 *
 *   - reset:      td5_game_init_race_session() (race init, per-slot loop)
 *   - accumulate: td5_physics_accumulate_metrics(), called once per LIVE race
 *                 sim tick from td5_game_run_race_frame() right after
 *                 td5_physics_tick() (i.e. NOT during the start countdown or
 *                 while paused — cars are stationary then).
 *   - read:       the post-race summary screen (td5_fe_race.c) via
 *                 td5_game_get_metrics() / td5_physics_get_metrics().
 *
 * Speeds are kept in the engine's RAW internal speed units (same unit as the
 * actor's longitudinal_speed >> 8 and the HUD speedometer's speed_raw); the
 * frontend converts to MPH/KPH for display with the speedometer formula
 * ((raw*256+625)/1252 MPH, (raw*256+389)/778 KPH).
 *
 * Stored as a standalone global array (NOT a g_td5 field) so adding it never
 * changes the g_td5 struct layout that other RE offsets depend on. */
typedef struct TD5_RaceMetrics {
    int32_t  top_speed;          /* running max of planar speed magnitude (raw units) */
    int64_t  speed_sum;          /* sum of per-tick planar speed magnitude (raw units) */
    int32_t  sample_ticks;       /* number of live sim ticks accumulated (avg = speed_sum / sample_ticks) */
    int32_t  collisions;         /* count of distinct collision events (V2V impact + hard wall hit) */
    int32_t  air_ticks;          /* number of live sim ticks spent fully airborne (all 4 wheels off ground) */
    int32_t  drifts;             /* count of sustained drifts (lateral slip held > 15 ticks = 0.5 s @30Hz) */

    /* --- transient per-tick edge-detect state (engine-private) --- */
    uint8_t  hit_this_tick;      /* set by collision sites during a sim tick (wall|V2V), cleared each accumulate */
    uint8_t  hit_prev_tick;      /* previous tick's hit state, for rising-edge collision counting */
    int32_t  drift_run_ticks;    /* current consecutive-drift tick run; a drift is counted once it crosses 15 */
    uint8_t  drift_counted;      /* 1 once the current drift run has already been counted (avoid double count) */
} TD5_RaceMetrics;

/** Per-racer-slot telemetry, indexed 0..TD5_MAX_RACER_SLOTS-1. Defined in
 *  td5_physics.c, reset in td5_game_init_race_session, read by the summary. */
extern TD5_RaceMetrics g_race_metrics[TD5_MAX_RACER_SLOTS];

/** Zero all metric slots (called at race init). Defined in td5_physics.c. */
void td5_physics_reset_metrics(void);

/** Accumulate one live sim tick of telemetry for every active racer slot.
 *  Called once per live race tick AFTER td5_physics_tick(). Defined in td5_physics.c. */
void td5_physics_accumulate_metrics(void);

/** Bounds-checked read of a slot's metrics (NULL if slot out of range). */
const TD5_RaceMetrics *td5_physics_get_metrics(int slot);
const TD5_RaceMetrics *td5_game_get_metrics(int slot);

/* Persist the in-game-configurable option keys (Display/Audio/GameOptions)
 * from g_td5.ini.* back to td5re.ini (the file s_ini_path resolved at boot).
 * Defined in main.c. Called by the option screens / pause sliders when the
 * user commits a change so it survives a relaunch (the boot-override in
 * td5_frontend.c re-applies g_td5.ini.* over Config.td5 every launch, so
 * without this write-back in-game changes were masked). [PART B, 2026-06-02] */
void td5_ini_persist_options(void);

/* S10 net-play: write a free-form string key back to td5re.ini (used to
 * persist the player nickname). */
void td5_ini_write_str(const char *section, const char *key, const char *value);

/* ========================================================================
 * Master Entry Points
 * ======================================================================== */

/** Initialize all modules in order. Returns 0 on failure. */
int  td5re_init(void);

/** Shutdown all modules in reverse order. */
void td5re_shutdown(void);

/** Run one frame of the main game loop. Returns 0 to continue, 1 to quit. */
int  td5re_frame(void);

/** Update the always-on FPS counter (call once per frame from the main loop). */
void td5_game_update_fps_overlay(void);

#endif /* TD5RE_H */
