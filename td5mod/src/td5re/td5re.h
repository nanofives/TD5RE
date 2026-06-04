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
void td5_track_tick(void);

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
    int  special_encounter_enabled;
    int  circuit_lap_count;
    int  checkpoint_timers_enabled;
    int  race_rule_variant;
    int  reverse_direction;
    int  dynamics_mode;  /* 0=arcade, 1=simulation */

    /* Quick Race player setup (infra to later replace the Two-Player menu).
     * num_human_players + num_ai_opponents <= TD5_MAX_RACER_SLOTS (16).
     * [PORT ENHANCEMENT 2026-06] N-way split-screen: up to TD5_MAX_HUMAN_PLAYERS
     * (9) local humans, each with its own viewport (see td5_game viewport ladder).
     * The original was hard-capped at 2 viewports / 6 racers; this path deviates
     * freely. Defaults (1 human + 5 AI = 6) reproduce the legacy single-race grid. */
    int  num_human_players;   /* 1..TD5_MAX_HUMAN_PLAYERS (9) */
    int  num_ai_opponents;    /* 0..TD5_MAX_RACER_SLOTS-1 (15) */

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
        /* Audio */
        int  sfx_volume;
        int  music_volume;
        int  sfx_mode;
        /* Game options */
        int  laps;
        int  checkpoint_timers;
        int  traffic;
        int  cops;
        int  difficulty;
        int  dynamics;
        int  collisions;
        /* AutoGearbox: 1 = automatic transmission (default; gear up/down keys
         * ignored), 0 = manual. Drives input bit 28 → actor+0x378 which orig
         * UpdatePlayerVehicleControlState @ 0x00402E60 gates the gear-shift
         * block on. The orig defaults to auto; in port the menu toggle is
         * not wired, so this INI key is the only way to switch to manual. */
        int  auto_gearbox;
        /* Per-player joystick selection (0 = keyboard, >=1 = 1-based enumerated
         * joystick index). Overrides the device index persisted in Config.td5.
         * The original supports up to 2 simultaneous joystick devices. */
        int  player1_joystick;
        int  player2_joystick;
        /* Defaults */
        int  default_car;
        int  default_track;
        int  default_game_type;
        int  default_opponents;   /* AutoRace AI-opponent count override; -1 = full grid (5) */
        int  circuit_minimap;     /* 1 = draw the in-race minimap on circuit tracks too (port enhancement; orig disabled it). 0 = faithful (no minimap on circuits) */
        int  default_players;     /* AutoRace local-human count override (N-way split test); -1 = schedule default */
        int  skip_intro;
        int  debug_overlay;
        int  debug_collisions;   /* 1 = draw wireframe of track wall/span geometry */
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
        /* Whole-state snapshot: tick-aligned dump of every actor byte +
         * a globals blob, emitted at the same instant as Frida's
         * UpdateRaceCameraTransitionTimer onEnter hook. See
         * re/analysis/whole_state_diff_design.md and
         * tools/diff_whole_state.py. Independent of [Trace] Modules. */
        int  whole_state_enabled;       /* [Trace] WholeState = 1 */
        int  whole_state_max_ticks;     /* [Trace] WholeStateMaxTicks = N (0 = unlimited) */
        /* Snapshot-replay harness (td5_trace_replay.c). Mode parsed from
         * [Trace] StateReplayMode = off|dump|inject|both into the integer
         * codes 0/1/2/3 (see td5_trace_replay.c RP_MODE_*). Paths come from
         * env vars TD5RE_STATE_REPLAY_{,DUMP_}PATH. */
        int  state_replay_mode;
        int  state_replay_start_frame;
        int  state_replay_end_frame;     /* 0 = unlimited */
        int  state_replay_max_frames;    /* 0 = unlimited (default 200) */
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
    } ini;

} TD5_GlobalState;

/** The single global state instance. */
extern TD5_GlobalState g_td5;

/* Persist the in-game-configurable option keys (Display/Audio/GameOptions)
 * from g_td5.ini.* back to td5re.ini (the file s_ini_path resolved at boot).
 * Defined in main.c. Called by the option screens / pause sliders when the
 * user commits a change so it survives a relaunch (the boot-override in
 * td5_frontend.c re-applies g_td5.ini.* over Config.td5 every launch, so
 * without this write-back in-game changes were masked). [PART B, 2026-06-02] */
void td5_ini_persist_options(void);

/* ========================================================================
 * Master Entry Points
 * ======================================================================== */

/** Initialize all modules in order. Returns 0 on failure. */
int  td5re_init(void);

/** Shutdown all modules in reverse order. */
void td5re_shutdown(void);

/** Run one frame of the main game loop. Returns 0 to continue, 1 to quit. */
int  td5re_frame(void);

#endif /* TD5RE_H */
