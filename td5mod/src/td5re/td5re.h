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
    int  wanted_mode_enabled;
    int  drag_race_enabled;
    int  traffic_enabled;
    int  special_encounter_enabled;
    int  circuit_lap_count;
    int  checkpoint_timers_enabled;
    int  race_rule_variant;
    int  reverse_direction;
    int  dynamics_mode;  /* 0=arcade, 1=simulation */

    /* Viewport */
    int  render_width;
    int  render_height;
    int  split_screen_mode;     /* 0=single, 1=horiz, 2=vert */
    int  viewport_count;

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
    int     ai_car_indices[6];  /* per-slot car index for AI racers (slot 1-5) */
    int     ai_car_variants[6]; /* per-slot color variant (0..3) */

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
        /* Defaults */
        int  default_car;
        int  default_track;
        int  default_game_type;
        int  skip_intro;
        int  debug_overlay;
        /* Trace */
        int  race_trace_enabled;
        int  race_trace_slot;
        int  race_trace_max_frames;
        int  auto_throttle;         /* 1 = force full throttle for slot 0 */
        int  trace_fast_forward;    /* N = inject N extra sim ticks per render frame
                                     * during trace capture (0 = real-time).
                                     * Mirrors the Frida-side sim budget clamp so the
                                     * diff-race skill runs in seconds instead of
                                     * minutes. */
        int  race_trace_max_sim_ticks; /* stop the trace after N sim ticks; 0 = no cap.
                                     * Paired with trace_fast_forward so the skill can
                                     * cap the port's workload at the same simulated
                                     * window the Frida side captures, instead of
                                     * burning time on sim ticks we'll never diff. */
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
        /* FrontendDraw: when 1, log every fe_draw_quad call to
         * log/frontend_draw_port.csv (screen, page, x, y, w, h, color, uvs).
         * Disabled by default — enable with FrontendDraw=1 in [Logging]. */
        int  log_frontend_draw;
        /* Logging — gates for the platform multi-file logger and the D3D
         * ddraw_wrapper log. Defaults preserve current behavior. Flip to
         * compare A/B perf without touching the build. */
        int  log_enabled;     /* master switch (1=on) */
        int  log_min_level;   /* 0=DEBUG 1=INFO 2=WARN 3=ERROR */
        int  log_frontend;    /* frontend.log (frontend, hud, save, input) */
        int  log_race;        /* race.log (game, physics, ai, track, camera, vfx) */
        int  log_engine;      /* engine.log (render, asset, platform, sound, net, fmv) */
        int  log_wrapper;     /* wrapper.log (D3D11 ddraw shim) */
    } ini;

} TD5_GlobalState;

/** The single global state instance. */
extern TD5_GlobalState g_td5;

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
