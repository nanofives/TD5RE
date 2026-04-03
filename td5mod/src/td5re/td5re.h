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
    int  race_rule_variant;
    int  reverse_direction;

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
    int     track_type;         /* TD5_TrackType */
    int     track_index;
    int     car_index;

    /* Frontend */
    int  frontend_screen_index;
    int  frontend_inner_state;
    int  frontend_frame_counter;

    /* Race state */
    int  race_end_fade_state;
    int  fade_direction;
    int  total_actor_count;

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
