/**
 * td5_game.c -- Main game loop, state machine, game flow
 *
 * Reimplements the 4-state game FSM: INTRO -> MENU -> RACE -> BENCHMARK
 * and the per-frame simulation/render pipeline.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "td5_game.h"
#include "td5_track.h"
#include "td5_fmv.h"
#include "td5_sound.h"
#include "td5_input.h"
#include "td5_ai.h"
#include "td5_asset.h"
#include "td5_physics.h"
#include "td5_render.h"
#include "td5_camera.h"
#include "td5_frontend.h"
#include "td5_hud.h"
#include "td5re.h"
#include "td5_platform.h"
#include "td5_net.h"
#include "td5_save.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================
 * Game State Globals (migrated from td5re_stubs.c — owned by this module)
 * ======================================================================== */

int     g_actorSlotForView[2]   = {0};
int     g_actorBaseAddr         = 0;
void   *g_actor_pool            = NULL;
void   *g_actor_base            = NULL;
uint8_t *g_actor_table_base     = NULL;
int     g_actor_slot_map[2]     = {0};
int     g_racer_count           = 0;
int     g_game_type             = 0;
int     g_split_screen_mode     = 0;
int     g_replay_mode           = 0;
int     g_wanted_mode_enabled   = 0;
int     g_special_encounter     = 0;
int     g_race_rule_variant     = 0;
float   g_instant_fps           = 30.0f;
uint32_t g_tick_counter         = 0;
int     g_special_render_mode   = 0;
int     g_pending_finish_timer  = 0;
int     g_race_end_state        = 0;
int32_t g_actor_best_lap        = 0;
int32_t g_actor_best_race       = 0;
void   *g_route_data            = NULL;

extern int   g_cameraTransitionActive;  /* td5_camera.c */
extern int   g_camWorldPos[2][3];       /* td5_camera.c -- per-viewport camera pos (24.8 fixed) */
extern float g_render_width_f;          /* td5_render.c */
extern float g_render_height_f;         /* td5_render.c */
extern int   g_track_is_circuit;        /* td5_track.c */
extern int   g_track_type_mode;         /* td5_track.c */

/* Checkpoint logging now routes through the centralized logger (race.log) */

/* ========================================================================
 * Original function addresses and implementation status
 *
 * 0x4493E0  entry                              -- N/A (CRT, not reimplemented)
 * 0x430A90  GameWinMain                        -- DONE (td5_game_init/tick/shutdown)
 * 0x442170  RunMainGameLoop                    -- DONE (td5_game_tick)
 * 0x42AA10  InitializeRaceSession              -- DONE (td5_game_init_race_session)
 * 0x42B580  RunRaceFrame                       -- DONE (td5_game_run_race_frame)
 * 0x42C8E0  ShowLegalScreens                   -- DONE (td5_game_show_legal_screens)
 * 0x442160  InitializeRaceVideoConfiguration   -- DONE (in td5_game_init)
 * 0x414740  InitializeFrontendResourcesAndState-- DONE (delegates to td5_frontend)
 * 0x414B50  RunFrontendDisplayLoop             -- DONE (delegates to td5_frontend)
 * 0x42C2B0  InitializeRaceViewportLayout       -- DONE (td5_game_init_viewport_layout)
 * 0x430CB0  ResetGameHeap                      -- DONE (delegates to td5_plat_heap_reset)
 * 0x40A2B0  AdvancePendingFinishState          -- DONE (advance_pending_finish_state)
 * 0x40A3D0  AccumulateVehicleSpeedBonusScore   -- DONE (accumulate_speed_bonus)
 * 0x40A440  DecayUltimateVariantTimer          -- DONE (decay_ultimate_timer)
 * 0x40A530  AdjustCheckpointTimersByDifficulty -- DONE (adjust_checkpoint_timers)
 * 0x42CC20  BeginRaceFadeOutTransition         -- DONE (td5_game_begin_fade_out)
 * 0x42CBE0  IsLocalRaceParticipantSlot         -- DONE (td5_game_is_local_participant)
 * 0x42CCD0  StoreRoundedVector3Ints            -- DONE (td5_game_store_rounded_vec3)
 * ======================================================================== */

/* ========================================================================
 * Key globals (from original binary)
 *
 * 0x4C3CE8  g_gameState (dword, enum)
 * 0x45D53C  g_appExref (pointer to DX::app object)
 * 0x474C00  g_introMoviePendingFlag
 * 0x474C04  g_frontendResourceInitPending
 * 0x495248  g_startRaceRequestFlag
 * 0x49524C  g_startRaceConfirmFlag
 * 0x4C3D80  g_raceEndFadeState
 * 0x45D5F4  tick decrement constant
 * 0x45D650  max sim tick budget (4.0f)
 * 0x4AAD60  g_gamePaused
 * ======================================================================== */

#define LOG_TAG "td5_game"
/* Original: g_cameraTransitionActive init=0xA000, -=0x100/tick, 160 ticks total
 * Level = timer / 0x2800; levels 4..0 → digits 5..1, then GO at level<0 */
#define TD5_COUNTDOWN_INIT    0xA000
#define TD5_COUNTDOWN_DECR    0x100
#define TD5_COUNTDOWN_LEVEL_DIV 0x2800

/* ========================================================================
 * Module-private state
 * ======================================================================== */

/* Fade system */
static float s_fade_accumulator;            /* 0.0 -> 255.0 */
static int   s_fade_direction_alternator;   /* toggles 0/1 per race */

/* Race completion */
static uint32_t s_post_finish_cooldown;     /* 0x483980: 0 = phase1, >0 = phase2 accumulator */

/* Per-actor race state (6 racer slots) */
typedef struct RaceSlotState {
    uint8_t  state;              /* 0=AI-inactive, 1=active, 2=completed, 3=disabled */
    uint8_t  companion_1;       /* 0=racing, 1=finished */
    uint8_t  companion_2;       /* 0=ok, 1=completed-ok, 2=DNF */
    uint8_t  reserved;
} RaceSlotState;

static RaceSlotState s_slot_state[TD5_MAX_RACER_SLOTS];

/* Per-actor metrics */
typedef struct ActorRaceMetric {
    int32_t  post_finish_metric_base;   /* cumulative timer at finish; 0 = not finished */
    int32_t  cumulative_timer;          /* running race timer (ticks) */
    int16_t  checkpoint_index;          /* current checkpoint or lap */
    uint8_t  checkpoint_bitmask;        /* circuit sector bitmask (4-bit) */
    int16_t  normalized_span;           /* position on track (forward progress) */
    int16_t  timer_ticks;               /* countdown timer for checkpoint mode */
    int32_t  accumulated_score;         /* points / speed bonus */
    int32_t  speed_bonus;               /* running speed bonus accumulator */
    int32_t  top_speed;                 /* max speed seen */
    int32_t  average_speed_raw;         /* for finish calculation */
    int16_t  display_position;          /* 0=1st .. 5=6th */
    int16_t  wanted_kills;              /* cop chase busts */
    int16_t  forward_speed;             /* current speed */
    int16_t  skid_factor;               /* current skid intensity */
    int16_t  contact_count;             /* collision count */
    int16_t  lap_split_times[8];        /* per-lap split deltas */
} ActorRaceMetric;

static ActorRaceMetric s_metrics[TD5_MAX_RACER_SLOTS];

/* Race order array (indices into slot table, sorted by position) */
static uint8_t s_race_order[TD5_MAX_RACER_SLOTS];

/* Results table (0x48d988 in original, 6 entries x 20 bytes) */
typedef struct RaceResultEntry {
    uint8_t  slot_flags;
    uint8_t  slot_index;
    int16_t  final_position;
    uint16_t pad1;
    uint16_t pad2;
    int32_t  primary_metric;     /* finish time */
    int32_t  secondary_metric;   /* accumulated points */
    uint8_t  wanted_kills;
    int16_t  speed_bonus;
    int16_t  top_speed;
} RaceResultEntry;

static RaceResultEntry s_results[TD5_MAX_RACER_SLOTS];

/* Checkpoint timing record (24 bytes = 12 x uint16, from binary at 0x46CBB0)
 * Loaded per-track via pointer table at 0x46CF6C (1-based index). */
typedef struct CheckpointRecord {
    uint16_t checkpoint_count;   /* always 5 in shipped data */
    uint16_t initial_time;       /* countdown start (8.8 fixed-point seconds) */
    struct {
        uint16_t span_threshold; /* span index where checkpoint triggers */
        uint16_t time_bonus;     /* time added on crossing (8.8 FP seconds) */
    } checkpoints[5];
} CheckpointRecord;

static CheckpointRecord s_active_checkpoint;

/* Hardcoded checkpoint timing table extracted from binary at 0x46CBB0.
 * 26 records (track_index 0..25), each 12 uint16s = 24 bytes.
 * Pointer table at 0x46CF6C uses 1-based indexing; here we use 0-based. */
static const uint16_t k_checkpoint_table[26][12] = {
    {5,25659,  869,15360, 1511,11520, 2061,15360, 2618,10240, 3074,    0}, /* 0:DragStrip */
    {5,17979,  826,11520, 1429, 5120, 1652, 7680, 1926,15360, 2516,    0}, /* 1:Jamaica */
    {5,20539,  768,17920, 1379,16640, 2090,16640, 2776,11520, 3221,    0}, /* 2:HouseOfBez */
    {5,17979,  623,12800, 1175,15360, 1751, 8960, 2181, 8960, 2552,    0}, /* 3:Newcastle */
    {5,17979,  747, 7680, 1006,12800, 1533,14080, 1978,17920, 2754,    0}, /* 4:Hawaii */
    {5,16699,  609,10240, 1029,10240, 1560,12800, 2140,16640, 2567,    0}, /* 5:Italy */
    {5,17979,  556,17920, 1113,14080, 1663,14080, 2305,23040, 3060,    0}, /* 6:Jordan */
    {5,20539,  715, 8960,  989, 8960, 1212,14080, 1815,12800, 2508,    0}, /* 7:Cheddar */
    {5,15419,  585,19200, 1271,20480, 1982,17920, 2593,17920, 3282,    0}, /* 8:Moscow */
    {5,15419,  466, 8960,  896,12800, 1472,14080, 2024,12800, 2528,    0}, /* 9:BlueRidge */
    {5,21819,  901,10240, 1346,11520, 1873, 7680, 2132,14080, 2755,    0}, /*10:Scotland */
    {5,15419,  519,15360, 1099,11520, 1630, 7680, 2050, 8960, 2523,    0}, /*11:Tokyo */
    {5,17979,  651,14080, 1128,12800, 1599,14080, 2115,11520, 2574,    0}, /*12:Sydney */
    {5,10299,  486,10240, 1057,11520, 1655, 8960, 2071,11520, 2658,    0}, /*13:Honolulu */
    {5,17979,  660,15360, 1297,14080, 1840,10240, 2193,12800, 2656,    0}, /*14:Munich */
    {5,23099,  629,10240, 1182,11520, 1608,12800, 2211,14080, 2644,    0}, /*15:Washington */
    {5,17979,  685,16640, 1446,11520, 1842,12800, 2281,17920, 2988,    0}, /*16:Kyoto */
    {5,16699,  606,15360, 1122,12800, 1593,16640, 2070,15360, 2610,    0}, /*17:Bern */
    {5,15419,  665,10240, 1081,12800, 1679,11520, 2250,11520, 2635,    0}, /*18:SanFrancisco */
    {5,17979,  583,11520,  936,14080, 1479,17920, 2116,14080, 2657,    0}, /*19:Keswick */
    {5,16699,  544,19200, 1147,11520, 1573,14080, 2126,14080, 2684,    0}, /*20:Cup21 */
    {5,23099,  827,12800, 1266,12800, 1662,19200, 2423,15360, 2989,    0}, /*21:Cup22 */
    {5,17979,  738, 7680, 1116,14080, 1707,10240, 2094,11520, 2649,    0}, /*22:Cup23 */
    {5,17979,  694, 8960, 1081,16640, 1672, 8960, 2050,17920, 2668,    0}, /*23:Cup24 */
    {5,30779,  106,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*24:Cup25 */
    {5,30779,   25,10240, 1511,11520, 2061,12800, 2618,14080, 3120,    0}, /*25:Cup26 */
};

/* Benchmark state */
static int   s_benchmark_image_load_pending;
static void *s_benchmark_image_data;

/* Position points tables */
static const int s_championship_points[TD5_MAX_RACER_SLOTS] = {15, 12, 10, 5, 4, 3};
static const int s_ultimate_points[TD5_MAX_RACER_SLOTS]     = {1000, 500, 250, 0, 0, 0};

/* Viewport layout */
typedef struct ViewportRect {
    int x, y, w, h;
} ViewportRect;

static ViewportRect s_viewports[2];

/* Replay / benchmark timing */
static uint32_t s_race_end_timer_start;
static int      s_replay_mode;
static int      s_race_countdown_ticks;
static int      s_race_countdown_state;
static int      s_pause_menu_active;
static int      s_pause_menu_cursor;   /* 0=View, 1=Music, 2=Sound, 3=Continue, 4=Exit */
static int      s_pause_input_done;    /* reset per-frame, set after first tick processes input */
static int      s_prev_esc_state;      /* edge detector for ESC key */

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static int  check_race_completion(uint32_t sim_delta);
static void build_results_table(void);
static void reset_results_table(void);
static void sort_results_by_time_asc(void);
static void sort_results_by_score_desc(void);
static void update_race_order(void);
static void advance_pending_finish_state(int slot, uint32_t sim_delta);
static void accumulate_speed_bonus(int slot);
static void decay_ultimate_timer(int slot);
static void adjust_checkpoint_timers(int slot);
static void display_loading_screen_tga(void);
static void reset_race_countdown(void);
static void tick_race_countdown(void);
static const char *td5_game_state_name(TD5_GameState state);

TD5_Actor *td5_game_get_actor(int slot)
{
    int total = td5_game_get_total_actor_count();

    if (!g_actor_table_base || slot < 0 || slot >= total) {
        return NULL;
    }

    return (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
}

int td5_game_get_total_actor_count(void)
{
    int total = g_td5.total_actor_count;

    if (total <= 0 && g_actor_table_base) {
        if (g_td5.time_trial_enabled) {
            total = 2;
        } else if (g_td5.traffic_enabled) {
            total = TD5_MAX_TOTAL_ACTORS;
        } else {
            total = TD5_MAX_RACER_SLOTS;
        }
    }

    if (total < 0) {
        return 0;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        return TD5_MAX_TOTAL_ACTORS;
    }

    return total;
}
static void set_countdown_indicator_state(int value);

/* ========================================================================
 * Module Init / Shutdown
 * ======================================================================== */

int td5_game_init(void) {
    /* Initialize game state machine (0x442170 entry, 0x430A90 GameWinMain setup) */
    g_td5.game_state = TD5_GAMESTATE_INTRO;
    g_td5.intro_movie_pending = g_td5.ini.skip_intro ? 0 : 1;
    g_td5.frontend_init_pending = 1;

    s_fade_accumulator = 0.0f;
    s_fade_direction_alternator = 0;
    s_post_finish_cooldown = 0;
    s_benchmark_image_load_pending = 1;
    s_benchmark_image_data = NULL;
    s_replay_mode = 0;
    s_race_end_timer_start = 0;
    s_race_countdown_ticks = 0;
    s_race_countdown_state = 0;

    memset(s_slot_state, 0, sizeof(s_slot_state));
    memset(s_metrics, 0, sizeof(s_metrics));
    memset(s_results, 0, sizeof(s_results));
    memset(s_viewports, 0, sizeof(s_viewports));
    g_td5.total_actor_count = 0;

    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        s_race_order[i] = (uint8_t)i;

    TD5_LOG_I(LOG_TAG, "Game module initialized, state=INTRO");
    return 1;
}

void td5_game_shutdown(void) {
    /* Release any lingering benchmark data */
    if (s_benchmark_image_data) {
        td5_plat_heap_free(s_benchmark_image_data);
        s_benchmark_image_data = NULL;
    }
    TD5_LOG_I(LOG_TAG, "Game module shut down");
}

/* ========================================================================
 * Main tick -- one frame of RunMainGameLoop (0x442170)
 *
 * 4-state FSM: INTRO -> MENU -> RACE -> BENCHMARK
 * Called once per frame from td5re_frame().
 * ======================================================================== */

int td5_game_tick(void) {
    /* Poll network subsystem (discovery, connection management) */
    td5_net_tick();

    switch (g_td5.game_state) {

    /* ------------------------------------------------------------------
     * GAMESTATE_INTRO (0): Play intro movie, init render, show legals,
     * then fall through to MENU.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_INTRO:
        /* Step 1: Play intro movie if pending and capable */
        if (g_td5.intro_movie_pending) {
            td5_game_play_intro_movie();
            if (g_td5.quit_requested) return 1;
            g_td5.intro_movie_pending = 0;
        }

        /* Step 2: Initialize render memory management and state */
        td5_render_init();

        /* Step 3: Show legal / splash screens (skip if SkipIntro is set) */
        if (!g_td5.ini.skip_intro)
            td5_game_show_legal_screens();

        /* Step 4: Transition to MENU (fallthrough) */
        TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                  td5_game_state_name(TD5_GAMESTATE_INTRO),
                  td5_game_state_name(TD5_GAMESTATE_MENU));
        g_td5.game_state = TD5_GAMESTATE_MENU;
        /* FALLTHROUGH */

    /* ------------------------------------------------------------------
     * GAMESTATE_MENU (1): Frontend resource init, display loop, race
     * start detection.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_MENU:
        /* Initialize frontend resources if pending */
        if (g_td5.frontend_init_pending) {
            td5_frontend_init_resources();
            g_td5.frontend_init_pending = 0;
        }

        /* Run one frame of the frontend display loop */
        td5_frontend_display_loop();

        /* Check if a race was requested */
        if (g_td5.race_requested || g_td5.race_confirmed) {
            g_td5.frontend_init_pending = 1;   /* re-init frontend on return */
            g_td5.race_confirmed = 0;
            g_td5.race_requested = 0;

            /* Heavy synchronous race session init (loading screen shown inside) */
            td5_game_init_race_session();

            TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                      td5_game_state_name(TD5_GAMESTATE_MENU),
                      td5_game_state_name(TD5_GAMESTATE_RACE));
            g_td5.game_state = TD5_GAMESTATE_RACE;
            return 0;
        }
        break;

    /* ------------------------------------------------------------------
     * GAMESTATE_RACE (2): Run one race frame. On completion, transition
     * to MENU or BENCHMARK.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_RACE: {
        int result = td5_game_run_race_frame();
        if (result != 0) {
            /* Race is over. Determine next state. */
            if (g_td5.benchmark_active) {
                TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                          td5_game_state_name(TD5_GAMESTATE_RACE),
                          td5_game_state_name(TD5_GAMESTATE_BENCHMARK));
                g_td5.game_state = TD5_GAMESTATE_BENCHMARK;
            } else {
                TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                          td5_game_state_name(TD5_GAMESTATE_RACE),
                          td5_game_state_name(TD5_GAMESTATE_MENU));
                g_td5.game_state = TD5_GAMESTATE_MENU;
                /* Always return to main menu (screen 5), never attract demo (screen 2) */
                td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            }
            return 0;
        }
        break;
    }

    /* ------------------------------------------------------------------
     * GAMESTATE_BENCHMARK (3): Display benchmark TGA, wait for keypress,
     * return to MENU.
     * ------------------------------------------------------------------ */
    case TD5_GAMESTATE_BENCHMARK: {
        g_td5.benchmark_active = 0;

        /* Load and display benchmark results TGA if pending */
        if (s_benchmark_image_load_pending) {
            /*
             * Original loads FPSName_exref TGA file:
             *   alloc 0x50000 for raw data + 0xA0000 for decoded pixels,
             *   decode via DX::ImageProTGA, blit 640x480 to primary surface.
             *
             * Source port: load the TGA through the asset pipeline and
             * present it via the platform render clear + present path.
             */
            void *bm_pixels = NULL;
            int bm_w = 0, bm_h = 0;
            if (td5_asset_load_png_to_buffer("../re/td5_png_clean/benchmark.png",
                                              TD5_COLORKEY_NONE, &bm_pixels, &bm_w, &bm_h)) {
                td5_plat_render_upload_texture(0, bm_pixels, bm_w, bm_h, 0);
                td5_plat_present(0);
                free(bm_pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "benchmark.png not found");
            }
            s_benchmark_image_load_pending = 0;
        }

        /* Poll for any keypress to dismiss */
        const uint8_t *kb = td5_plat_input_get_keyboard();
        if (kb) {
            for (int k = 0; k < 256; k++) {
                if (kb[k]) {
                    /* Keypress detected: return to MENU */
                    s_benchmark_image_load_pending = 1;
                    TD5_LOG_I(LOG_TAG, "State transition: %s -> %s",
                              td5_game_state_name(TD5_GAMESTATE_BENCHMARK),
                              td5_game_state_name(TD5_GAMESTATE_MENU));
                    g_td5.game_state = TD5_GAMESTATE_MENU;
                    break;
                }
            }
        }
        break;
    }

    default:
        TD5_LOG_E(LOG_TAG, "Unknown game state %d", g_td5.game_state);
        g_td5.game_state = TD5_GAMESTATE_MENU;
        break;
    }

    return g_td5.quit_requested;
}

/* ========================================================================
 * State Machine Accessors
 * ======================================================================== */

void td5_game_set_state(TD5_GameState state) {
    g_td5.game_state = state;
}

TD5_GameState td5_game_get_state(void) {
    return g_td5.game_state;
}

int td5_game_get_player_lap(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return (int)s_metrics[slot].checkpoint_index;
}

/* Returns cumulative race timer ticks (30/sec) for lap_index 0,
 * or the split time for lap_index 1-8. Used by HUD. */
int32_t td5_game_get_race_timer(int slot, int lap_index)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (lap_index == 0) return s_metrics[slot].cumulative_timer;
    if (lap_index >= 1 && lap_index <= 8)
        return (int32_t)s_metrics[slot].lap_split_times[lap_index - 1];
    return 0;
}

/* ========================================================================
 * InitializeRaceSession (0x42AA10)
 *
 * 33-step synchronous race bootstrap. The loading screen is displayed
 * at step 1, then all heavy asset loading follows. No progress bar.
 * ======================================================================== */

int td5_game_init_race_session(void) {
    #define CK(n) TD5_LOG_I(LOG_TAG, "CK: %s", n)
    CK("ck0_start");
    TD5_LOG_I(LOG_TAG, "InitializeRaceSession: begin");

    /* ---- Step 1: Display random loading screen TGA (rand()%20) ---- */
    display_loading_screen_tga();
    TD5_LOG_I(LOG_TAG, "InitRace step 1/19: loading screen displayed for track=%d",
              g_td5.track_index);
    CK("ck1_after_loading_screen");

    /* ---- Step 2: Reset game heap (0x430CB0, 24 MB pool) ---- */
    td5_plat_heap_reset();
    TD5_LOG_I(LOG_TAG, "InitRace step 2/19: heap reset complete");
    CK("ck2_after_heap_reset");

    /* ---- Step 3: Configure race slot states (player/AI/disabled) ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_slot_state[i].state       = (i == 0) ? 1 : 0;  /* slot 0 = player */
        s_slot_state[i].companion_1 = 0;
        s_slot_state[i].companion_2 = 0;
        s_slot_state[i].reserved    = 0;
    }
    /* If split-screen, slot 1 is also a player */
    if (g_td5.split_screen_mode > 0) {
        s_slot_state[1].state = 1;
    }
    /* Mark unused racer slots as disabled based on the current mode */
    {
        int racer_slot_count = g_td5.time_trial_enabled ? 2 : TD5_MAX_RACER_SLOTS;
        for (int i = racer_slot_count; i < TD5_MAX_RACER_SLOTS; i++) {
            s_slot_state[i].state = 3;  /* disabled */
        }
    }
    TD5_LOG_I(LOG_TAG,
              "InitRace step 3/19: race slots configured split=%d time_trial=%d traffic=%d",
              g_td5.split_screen_mode, g_td5.time_trial_enabled, g_td5.traffic_enabled);
    g_game_type = g_td5.game_type;
    g_split_screen_mode = g_td5.split_screen_mode;
    g_replay_mode = s_replay_mode;
    g_racer_count = g_td5.time_trial_enabled ? 2 : TD5_MAX_RACER_SLOTS;

    /* ---- Step 4: Load track runtime data ---- */
    /* NOTE: td5_asset_load_level sets g_td5.track_type from LEVELINF.DAT,
     * so g_track_is_circuit / g_track_type_mode must be derived after this call. */
    td5_asset_load_level(g_td5.track_index);
    g_track_is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    g_track_type_mode = g_track_is_circuit ? 1 : 0;
    TD5_LOG_I(LOG_TAG, "InitRace step 4/19: level runtime loaded track=%d is_circuit=%d", g_td5.track_index, g_track_is_circuit);
    CK("ck4_after_load_level");

    /* ---- Step 4b: Initialize race sound resources ---- */
    td5_sound_init_race_resources();

    /* ---- Step 5: Load vehicle assets and sound banks for all active slots ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            int car_for_slot = (i == 0) ? g_td5.car_index : g_td5.ai_car_indices[i];
            td5_asset_load_vehicle(car_for_slot, i);

            /* Load per-vehicle sound bank (Drive.wav, Rev.wav/Reverb.wav, Horn.wav).
             * Slot 0 is the local player and uses Reverb.wav (is_reverb=1). */
            const char *car_zip = td5_asset_get_car_zip_path(car_for_slot);
            if (car_zip) {
                td5_sound_load_vehicle_bank(car_zip, i, (i == 0) ? 1 : 0);
            }

            TD5_LOG_I(LOG_TAG, "InitRace step 5/19: vehicle asset loaded slot=%d car_index=%d",
                      i, car_for_slot);
        }
    }

    /* ---- Step 6: Bind track strip runtime pointers ---- */
    /* (Internal to td5_asset_load_level -- strip data is patched in place) */
    TD5_LOG_I(LOG_TAG, "InitRace step 6/19: track strip runtime pointers bound");

    /* ---- Step 7: Parse MODELS.DAT from level ZIP ---- */
    /* (Loaded as part of td5_asset_load_level) */
    TD5_LOG_I(LOG_TAG, "InitRace step 7/19: MODELS.DAT parsed from level assets");

    /* ---- Step 8: Load track textures ---- */
    td5_asset_load_track_textures(g_td5.track_index);
    TD5_LOG_I(LOG_TAG, "InitRace step 8/19: track textures loaded for track=%d",
              g_td5.track_index);

    /* ---- Step 9: Load sky mesh (SKY.PRR from STATIC.ZIP) ---- */
    /* (Loaded as part of td5_asset_load_track_textures static resource pass) */
    TD5_LOG_I(LOG_TAG, "InitRace step 9/19: sky mesh/static resources prepared");

    /* ---- Step 10: Initialize race vehicle runtime ---- */
    /* Initialize per-actor race metrics */
    memset(s_metrics, 0, sizeof(s_metrics));
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_metrics[i].display_position = (int16_t)i;
        s_race_order[i] = (uint8_t)i;
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 10/19: race metrics/runtime arrays reset");

    /* ---- Step 11: Allocate actors and init vehicle/AI runtime ---- */
    {
        static uint8_t s_actor_memory[TD5_ACTOR_STRIDE * TD5_MAX_TOTAL_ACTORS];
        int spawn_count = g_td5.time_trial_enabled ? 2 :
                          (g_td5.traffic_enabled ? TD5_MAX_TOTAL_ACTORS : TD5_MAX_RACER_SLOTS);
        int racer_count = (spawn_count > TD5_MAX_RACER_SLOTS)
                          ? TD5_MAX_RACER_SLOTS : spawn_count;

        memset(s_actor_memory, 0, sizeof(s_actor_memory));
        g_actorBaseAddr = (int)(uintptr_t)s_actor_memory;
        g_actor_pool = s_actor_memory;
        g_actor_base = s_actor_memory;
        g_actor_table_base = s_actor_memory;
        g_td5.total_actor_count = spawn_count;
        td5_ai_bind_actor_table(s_actor_memory);

        /* Original order (0x42AFE2-0x42AFE7): vehicle + AI runtime init
         * BEFORE actor placement (step 22). */
        td5_physics_init_vehicle_runtime();
        td5_ai_init_race_actor_runtime();

        /* ---- Step 11b: Position racer actors on grid ---- */
        /* Grid patterns from InitializeRaceSession (0x42B07B-0x42B225):
         *   Circuit (0x42B110): paired rows 6 spans apart
         *   Non-circuit (0x42B174): staggered 3 spans apart */
        static const int8_t s_circuit_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -6, -6, -12, -12, -18, -18
        };
        static const int8_t s_staggered_span_offsets[TD5_MAX_RACER_SLOTS] = {
            -3, -6, -9, -12, -15, -18
        };
        static const uint8_t s_racer_lanes[TD5_MAX_RACER_SLOTS] = {
            1, 2, 1, 2, 1, 2
        };

        /* Per-track start span: indexed by LEVEL NUMBER (1-based, from
         * td5_asset_level_number), NOT schedule slot.
         * Circuit: 16-bit at 0x46CBB0[level*24+4]; non-circuit: byte at 0x466F6F[level]. */
        static const uint16_t s_circuit_start_span[40] = {
            /*  0 */ 0,
            /*  1 */ 869,  826,  768,  623,  747,  609,  556,  715,
            /*  9 */ 585,  466,  901,  519,  651,  486,  660,  629,
            /* 17 */ 685,  606,  665,  583,
            /* 21 */ 544,  827,  738,  694,  106,   25,  119,
            /* 28 */  56,  116,  204,  204,  106,   25,  119,
            /* 35 */  56,  116,   47,   47,   35
        };
        static const uint8_t s_noncircuit_start_span[40] = {
            /*  0 */ 0,
            /*  1 */ 114, 134,  79, 119, 124, 136, 100, 125,
            /*  9 */ 140,  95, 125,  92, 111, 101, 119,  71,
            /* 17 */ 119, 147,  78, 120,
            /* 21 */ 111, 120, 120, 139,   0,   0,   0,
            /* 28 */   0,   0,   0,   0,   0,   0,   0,
            /* 35 */   0,   0,   0,   0,   0
        };

        const int8_t *span_offsets = g_track_is_circuit
                                     ? s_circuit_span_offsets
                                     : s_staggered_span_offsets;

        int track_span_count = td5_track_get_span_count();
        int level_num = td5_asset_level_number(g_td5.track_index);
        int start_span;
        if (level_num >= 1 && level_num <= 39) {
            start_span = g_track_is_circuit
                         ? (int)s_circuit_start_span[level_num]
                         : (int)s_noncircuit_start_span[level_num];
        } else {
            start_span = (track_span_count > 0) ? track_span_count : 1;
        }
        if (start_span <= 0)
            start_span = (track_span_count > 0) ? track_span_count : 1;
        TD5_LOG_I(LOG_TAG, "Grid start: slot=%d level=%d circuit=%d start_span=%d span_count=%d",
                  g_td5.track_index, level_num, g_track_is_circuit, start_span, track_span_count);

        for (int slot = 0; slot < racer_count; ++slot) {
            uint8_t *actor = s_actor_memory + slot * TD5_ACTOR_STRIDE;
            int span_index;
            int world_x = 0;
            int world_y = 0;
            int world_z = 0;
            int sub_lane = s_racer_lanes[slot];
            TD5_StripSpan *sp;

            if (track_span_count > 0) {
                span_index = start_span + span_offsets[slot];
                while (span_index < 0)
                    span_index += track_span_count;
                while (span_index >= track_span_count)
                    span_index -= track_span_count;
            } else {
                span_index = 1;
            }

            sp = td5_track_get_span(span_index);
            if (!sp) {
                span_index = 1;
                sp = td5_track_get_span(span_index);
            }
            if (!sp)
                continue;

            *(int16_t *)(actor + 0x080) = (int16_t)span_index;
            *(int16_t *)(actor + 0x082) = (int16_t)span_index;
            *(int16_t *)(actor + 0x084) = (int16_t)span_index;
            *(int16_t *)(actor + 0x086) = (int16_t)span_index;
            actor[0x08C] = (uint8_t)sub_lane;

            if (!td5_track_get_span_lane_world(span_index, sub_lane, &world_x, &world_y, &world_z)) {
                world_x = sp->origin_x;
                world_y = sp->pad_10;
                world_z = sp->origin_z;
            }

            /* Actor positions are 24.8 fixed-point; track functions return
             * integer coords, so shift left by 8 to convert to FP. */
            *(int32_t *)(actor + 0x1FC) = world_x << 8;
            *(int32_t *)(actor + 0x200) = world_y << 8;
            *(int32_t *)(actor + 0x204) = world_z << 8;

            actor[0x375] = (uint8_t)slot;
            actor[0x37B] = 1;
            td5_track_compute_heading((TD5_Actor *)actor);
            td5_physics_reset_actor_state((TD5_Actor *)actor);
            TD5_LOG_I(LOG_TAG,
                      "Actor spawn: slot=%d span=%d pos=(%d,%d,%d) state=%d lane=%d",
                      slot, span_index,
                      *(int32_t *)(actor + 0x1FC),
                      *(int32_t *)(actor + 0x200),
                      *(int32_t *)(actor + 0x204),
                      s_slot_state[slot].state,
                      sub_lane);
        }

        TD5_LOG_I(LOG_TAG, "InitRace step 11/19: actors spawned and runtime bound count=%d",
                  spawn_count);

        TD5_LOG_I(LOG_TAG, "Actors spawned: base=%p count=%d racers=%d",
                  (void *)s_actor_memory, g_td5.total_actor_count, racer_count);
    }

    /* ---- Step 12: Open input recording/playback ---- */
    if (s_replay_mode) {
        td5_input_read_open("replay.td5");
        td5_input_set_playback_active(1);
    } else {
        td5_input_write_open("replay.td5");
        td5_input_set_playback_active(0);
    }
    TD5_LOG_I(LOG_TAG, "InitRace step 12/19: input %s initialized",
              s_replay_mode ? "playback" : "recording");

    CK("ck13_before_ambient");
    /* ---- Step 13: Load ambient sounds ---- */
    td5_sound_load_ambient();
    TD5_LOG_I(LOG_TAG, "InitRace step 13/19: ambient sounds loaded");
    CK("ck13_after_ambient");

    /* ---- Step 14: Initialize particles, smoke, tire tracks, weather ---- */
    td5_vfx_init();
    TD5_LOG_I(LOG_TAG, "InitRace step 14/19: VFX systems initialized");

    /* ---- Step 15: Configure force feedback + input mapping ---- */
    td5_input_set_active_players(g_td5.split_screen_mode > 0 ? 2 : 1);
    td5_input_ff_init();
    td5_input_reset_accumulators();
    td5_input_reset_buffers();
    TD5_LOG_I(LOG_TAG, "InitRace step 15/19: force feedback and input buffers initialized (players=%d)",
              g_td5.split_screen_mode > 0 ? 2 : 1);

    /* ---- Step 16: Start CD audio track ---- */
    td5_sound_cd_play(g_td5.track_index % 10 + 1);
    TD5_LOG_I(LOG_TAG, "InitRace step 16/19: CD audio started track=%d",
              g_td5.track_index % 10 + 1);

    CK("ck17_before_viewport");
    /* ---- Step 17: Initialize 3D render state + viewport layout ---- */
    td5_render_reset_texture_cache();
    td5_game_init_viewport_layout();
    g_actorSlotForView[0] = 0;
    g_actorSlotForView[1] = (g_td5.split_screen_mode > 0 && g_td5.total_actor_count > 1) ? 1 : 0;
    g_actor_slot_map[0] = g_actorSlotForView[0];
    g_actor_slot_map[1] = g_actorSlotForView[1];
    TD5_LOG_I(LOG_TAG, "InitRace step 17/19: render state and viewport layout initialized views=%d",
              g_td5.viewport_count);
    CK("ck17_after_viewport");

    /* ---- Step 18: Upload race texture pages to GPU ---- */
    td5_asset_load_race_texture_pages();
    TD5_LOG_I(LOG_TAG, "InitRace step 18/19: race texture pages uploaded");

    /* ---- Step 19: Initialize HUD, pause menu overlay ---- */
    #define DBG_WRITE(msg) TD5_LOG_I(LOG_TAG, "Step19: %s", msg)
    DBG_WRITE("19a_before_overlay");
    td5_hud_init_overlay_resources(1, 0);
    DBG_WRITE("19b_before_layout");
    td5_hud_init_layout(g_td5.split_screen_mode);
    DBG_WRITE("19c_before_minimap");
    td5_hud_init_minimap_layout();
    DBG_WRITE("19d_before_font");
    td5_hud_init_font_atlas();
    DBG_WRITE("19e_before_pause");
    td5_hud_init_pause_menu(0);
    DBG_WRITE("19f_complete");
    #undef DBG_WRITE
    td5_camera_set_preset(0);
    TD5_LOG_I(LOG_TAG, "InitRace step 19/19: HUD and pause menu initialized");

    /* ---- Load sky texture ---- */
    {
        char sky_path[256];
        int level_num = td5_asset_level_number(g_td5.track_index);
        snprintf(sky_path, sizeof(sky_path),
                 "../re/td5_png_clean/levels/level%03d/FORWSKY.png", level_num);
        td5_render_load_sky(sky_path);
    }

    /* ---- Step 20: Load checkpoint timing from hardcoded table (0x46CBB0) ---- */
    {
        int tidx = g_td5.track_index;
        memset(&s_active_checkpoint, 0, sizeof(s_active_checkpoint));
        if (tidx >= 0 && tidx < 26) {
            memcpy(&s_active_checkpoint, k_checkpoint_table[tidx], 24);
            TD5_LOG_I(LOG_TAG, "Checkpoint record loaded: track=%d count=%d initial_time=%u",
                      tidx, (int)s_active_checkpoint.checkpoint_count,
                      (unsigned)s_active_checkpoint.initial_time);
            for (int ci = 0; ci < (int)s_active_checkpoint.checkpoint_count && ci < 5; ci++) {
                TD5_LOG_I(LOG_TAG, "  checkpoint[%d]: span_threshold=%u time_bonus=%u",
                          ci,
                          (unsigned)s_active_checkpoint.checkpoints[ci].span_threshold,
                          (unsigned)s_active_checkpoint.checkpoints[ci].time_bonus);
            }
        } else {
            TD5_LOG_W(LOG_TAG, "Track index %d out of range, no checkpoint data", tidx);
        }
    }

    /* ---- Step 21: Adjust checkpoint timers by difficulty ---- */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 1) {   /* human player */
            if (g_td5.checkpoint_timers_enabled) {
                adjust_checkpoint_timers(i);
            } else {
                s_metrics[i].timer_ticks = 0x7FFF;  /* disable: max timer */
            }
        }
    }

    /* ---- Reset race state ---- */
    g_td5.race_end_fade_state = 0;
    g_td5.paused = 1;              /* start paused for countdown */
    s_pause_menu_active = 0;       /* clear stale pause menu from previous race */
    s_prev_esc_state = 1;          /* suppress false ESC edge on first frame */
    g_td5.sim_tick_budget = 0.0f;
    g_td5.sim_time_accumulator = 0;
    g_td5.simulation_tick_counter = 0;
    g_td5.frame_prev_timestamp = td5_plat_time_ms();
    s_fade_accumulator = 0.0f;
    s_post_finish_cooldown = 0;
    s_race_end_timer_start = 0;

    /* Reset results table */
    reset_results_table();

    /* Notify sound that race is starting */
    td5_sound_set_race_end(0);

    reset_race_countdown();

    TD5_LOG_I(LOG_TAG, "InitializeRaceSession: complete (%d actors)",
              g_td5.total_actor_count);
    return 1;
}

/* ========================================================================
 * RunRaceFrame (0x42B580)
 *
 * Fixed-timestep simulation loop + render pipeline + audio tick.
 * Returns 0 = racing continues, 1 = race over (fade complete).
 * ======================================================================== */

int td5_game_run_race_frame(void) {
    int i;
    /* ---- Update frame timing ---- */
    td5_game_update_frame_timing();

    /* ---- Race completion check (before sim loop) ---- */
    if (g_td5.race_end_fade_state == 0) {
        int completion = check_race_completion(
            (uint32_t)(g_td5.normalized_frame_dt * 65536.0f));
        if (completion) {
            g_td5.race_end_fade_state = 1;

            /* Select fade direction based on viewport layout */
            td5_game_begin_fade_out(g_td5.split_screen_mode);
        }
    }

    /* ---- Fade-out accumulation ---- */
    if (g_td5.race_end_fade_state > 0) {
        /* Replay timeout: force fade after 45 seconds */
        if (s_replay_mode && s_race_end_timer_start > 0) {
            uint32_t elapsed = td5_plat_time_ms() - s_race_end_timer_start;
            if (elapsed > 45000) {
                s_fade_accumulator = 255.0f;
            }
        }

        /* Accumulate fade */
        s_fade_accumulator += g_td5.normalized_frame_dt * 2.0f;
        if (s_fade_accumulator >= 255.0f) {
            s_fade_accumulator = 255.0f;

            /* Fade complete: release all race resources and exit */
            td5_game_release_race_resources();

            return 1;  /* race over */
        }
    }

    /* ---- Fixed-timestep simulation loop ---- */
    /* Drain sim_time_accumulator in 0x10000 steps, max 4 ticks per frame */
    int ticks_this_frame = 0;
    s_pause_input_done = 0;  /* allow pause input once this frame */

    {
        static uint32_t s_frame_diag_ctr = 0;
        if ((s_frame_diag_ctr++ % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "race_frame: accum=0x%X paused=%d pause_menu=%d trans=%d",
                      g_td5.sim_time_accumulator, g_td5.paused,
                      s_pause_menu_active, g_cameraTransitionActive);
        }
    }

    while (g_td5.sim_time_accumulator > 0xFFFF && ticks_this_frame < 4) {
        /* --- Input polling --- */
        td5_input_poll_race_session();

        /* --- Camera angle caching (per viewport) --- */
        /* td5_camera_cache_vehicle_angles for primary and secondary players */
        td5_camera_tick();

        /* --- Pause menu (ESC toggles) --- */
        int esc_now = td5_plat_input_key_pressed(0x01);
        int esc_edge = (esc_now && !s_prev_esc_state);
        int pause_menu_was_active = s_pause_menu_active;
        s_prev_esc_state = esc_now;
        if (esc_edge && !s_pause_menu_active) {
            s_pause_menu_active = 1;
            s_pause_menu_cursor = 3;  /* default to CONTINUE */
        }
        if (s_pause_menu_active) {
            /* Process pause menu input ONCE per frame (not per tick) to avoid
             * multiple edge triggers from the sim tick loop. */
            if (!s_pause_input_done) {
                s_pause_input_done = 1;

                static int s_prev_down = 0, s_prev_up = 0;
                static int s_prev_left = 0, s_prev_right = 0;
                static int s_prev_enter = 0;
                int key_down  = td5_plat_input_key_pressed(0xD0);
                int key_up    = td5_plat_input_key_pressed(0xC8);
                int key_left  = td5_plat_input_key_pressed(0xCB);
                int key_right = td5_plat_input_key_pressed(0xCD);
                int key_enter = td5_plat_input_key_pressed(0x1C);

                /* Navigation: 5 selectable items (View / Music / Sound / Continue / Exit) */
                if (key_down  && !s_prev_down)  s_pause_menu_cursor = (s_pause_menu_cursor + 1) % 5;
                if (key_up    && !s_prev_up)    s_pause_menu_cursor = (s_pause_menu_cursor + 4) % 5;

                /* Left/right adjusts sliders for rows 0-2 (View / Music / Sound).
                 * CONTINUOUS while held (not edge-triggered) — matches original binary
                 * RunAudioOptionsOverlay (0x43BF70): slider[cursor] += 0.02 per frame. */
                if (s_pause_menu_cursor < 3) {
                    if (key_right) {
                        if (s_pause_menu_cursor == 0)      td5_save_set_view_distance(td5_save_get_view_distance() + 0.02f);
                        else if (s_pause_menu_cursor == 1) td5_save_set_music_volume(td5_save_get_music_volume() + 2);
                        else                               td5_save_set_sfx_volume(td5_save_get_sfx_volume() + 2);
                    }
                    if (key_left) {
                        if (s_pause_menu_cursor == 0)      td5_save_set_view_distance(td5_save_get_view_distance() - 0.02f);
                        else if (s_pause_menu_cursor == 1) td5_save_set_music_volume(td5_save_get_music_volume() - 2);
                        else                               td5_save_set_sfx_volume(td5_save_get_sfx_volume() - 2);
                    }
                }

                /* Confirm (Enter) */
                if (key_enter && !s_prev_enter) {
                    if (s_pause_menu_cursor == 3) {
                        s_pause_menu_active = 0;
                    } else if (s_pause_menu_cursor == 4) {
                        s_pause_menu_active = 0;
                        td5_game_release_race_resources();
                        td5_game_set_state(TD5_GAMESTATE_MENU);
                        return 1;
                    }
                }

                s_prev_down = key_down; s_prev_up = key_up;
                s_prev_left = key_left; s_prev_right = key_right;
                s_prev_enter = key_enter;
            }

            /* ESC again = continue */
            if (esc_edge && pause_menu_was_active) {
                s_pause_menu_active = 0;
            }

            /* Update graphical overlay (SELBOX + sliders).
             * Row 0=View (stub: 0.5), Row 1=Music, Row 2=Sound */
            float view_frac  = td5_save_get_view_distance();
            float music_frac = (float)td5_save_get_music_volume()  / 100.0f;
            float sfx_frac   = (float)td5_save_get_sfx_volume()   / 100.0f;
            td5_hud_update_pause_overlay(s_pause_menu_cursor, view_frac, music_frac, sfx_frac);

            g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
            ticks_this_frame++;
            continue;
        }

        if (g_td5.paused) {
            tick_race_countdown();
            /* Poll input during countdown so player can rev the engine.
             * Physics runs with g_game_paused=1, which only updates
             * engine RPM (no vehicle movement). */
            td5_physics_set_paused(1);
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
                if (s_slot_state[i].state == 1)
                    td5_input_update_player_control(i);
            }
            td5_physics_tick();
            td5_track_tick();
            g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
            ticks_this_frame++;
            continue;
        }
        td5_physics_set_paused(0);

        /* Input record/playback is handled inside td5_input_poll_race_session(). */

        /* --- Per-slot player control update --- */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 1) {   /* active player */
                td5_input_update_player_control(i);
            }
        }

        /* --- Core simulation: physics, contacts, AI --- */
        td5_physics_tick();

        /* --- Track update (tire marks, wrap normalization) --- */
        td5_track_tick();

        /* --- VFX tick (tire tracks) --- */
        td5_vfx_tick();

        /* --- Update race order (bubble sort by span position) --- */
        update_race_order();

        /* --- Per-viewport camera update --- */
        /* (Already handled in td5_camera_tick above for both viewports) */

        /* --- Per-actor race progression --- */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 3) continue; /* disabled */
            advance_pending_finish_state(i, TD5_TICK_ACCUMULATOR_ONE);
            accumulate_speed_bonus(i);
            decay_ultimate_timer(i);
        }

        /* --- Consume one tick --- */
        g_td5.sim_time_accumulator -= TD5_TICK_ACCUMULATOR_ONE;
        g_td5.simulation_tick_counter++;
        ticks_this_frame++;
    }

    if ((g_td5.simulation_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "Race frame timing: dt=%.3f fps=%.2f ticks_this_frame=%d paused=%d pause_menu=%d countdown_indicator=%d countdown_timer=0x%X fade=%d",
                  g_td5.normalized_frame_dt,
                  g_td5.instant_fps,
                  ticks_this_frame,
                  g_td5.paused,
                  s_pause_menu_active,
                  s_race_countdown_state,
                  g_cameraTransitionActive,
                  g_td5.race_end_fade_state);
        {
            TD5_Actor *actor0 = td5_game_get_actor(0);
            if (actor0) {
                uint8_t *a0 = (uint8_t *)actor0;
                TD5_LOG_D(LOG_TAG,
                          "Race actor0: pos=(%d,%d,%d) speed=%d gear=%d",
                          *(int32_t *)(a0 + 0x1CC),
                          *(int32_t *)(a0 + 0x1D0),
                          *(int32_t *)(a0 + 0x1D4),
                          *(int32_t *)(a0 + 0x208),
                          *(int32_t *)(a0 + 0x224));
            }
        }
    }

    /* ---- Rendering pipeline ---- */
    /* ---- Rendering pipeline ----*/

    /* Begin scene */
    td5_render_begin_scene();

    /* For each viewport: camera setup, sky, track, actors, vfx, hud */
    for (int vp = 0; vp < g_td5.viewport_count; vp++) {
        /* Set viewport rectangle */
        td5_plat_render_set_viewport(
            s_viewports[vp].x, s_viewports[vp].y,
            s_viewports[vp].w, s_viewports[vp].h);

        /* Camera transition state */
        {
            static int s_cam_debug_logged = 0;
            TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[vp]);

            if (!actor && g_actor_table_base) {
                int slot = g_actorSlotForView[vp];
                int total = td5_game_get_total_actor_count();

                if (slot >= 0 && slot < total) {
                    actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
                }
            }

            if (!s_cam_debug_logged) {
                TD5_LOG_I(LOG_TAG,
                          "Camera first frame: actor=%p base=%p count=%d slot=%d",
                          (void *)actor,
                          (void *)g_actor_table_base,
                          td5_game_get_total_actor_count(),
                          g_actorSlotForView[vp]);
                s_cam_debug_logged = 1;
            }
        }
        td5_camera_update_transition_state(vp, vp);

        /* Configure projection for this viewport */
        td5_render_configure_projection(s_viewports[vp].w, s_viewports[vp].h);

        /* Sky dome */
        td5_render_advance_sky_rotation();
        td5_render_advance_billboard_anims();
        /* Track spans from display list */
        /* (td5_render_span_display_list called internally by render_frame) */

        /* Render race actors for this view */
        td5_render_actors_for_view(vp);

        /* VFX: tire tracks, particles */
        /* (Rendered as part of translucent pipeline) */
        td5_render_flush_translucent();
        td5_render_flush_projected_buckets();

        /* HUD overlay for this viewport */
        td5_hud_draw_status_text(vp, vp);
        td5_hud_render_minimap(vp);
    }

    /* Full-screen HUD overlay (speedometer, lap counter, etc.) */
    td5_hud_render_overlays(g_td5.sim_tick_budget);

    /* Pause overlay: draw panel then queue text labels on top */
    if (s_pause_menu_active) {
        td5_hud_draw_pause_overlay();
        /* Queue text AFTER draw_pause_overlay so flush_text renders on top.
         * Row N pixel_y = cy_px - 52 + N*16 (matches binary layout). */
        {
            int cx_px = (int)(g_render_width_f * 0.5f);
            int cy_px = (int)(g_render_height_f * 0.5f);
            int lx = cx_px - 124;   /* left-aligned: cx - half_w(128) + 4 */
            td5_hud_queue_text(0, cx_px, cy_px - 52,      1, "PAUSED");
            td5_hud_queue_text(0, lx, cy_px - 36,         0, "VIEW");
            td5_hud_queue_text(0, lx, cy_px - 20,         0, "MUSIC");
            td5_hud_queue_text(0, lx, cy_px - 4,          0, "SOUND");
            td5_hud_queue_text(0, cx_px, cy_px + 12,      1, "CONTINUE");
            td5_hud_queue_text(0, cx_px, cy_px + 28,      1, "EXIT");
        }
    }

    td5_hud_flush_text();

    /* ---- Audio tick ---- */

    /* Feed camera position into the sound system as listener position.
     * g_camWorldPos is in 24.8 fixed-point, which is the same coordinate
     * space td5_sound expects (matching actor world_pos). */
    for (int vp = 0; vp < (g_td5.split_screen_mode ? 2 : 1); vp++) {
        td5_sound_set_listener_pos(vp,
            g_camWorldPos[vp][0],
            g_camWorldPos[vp][1],
            g_camWorldPos[vp][2]);
    }

    /* Feed per-vehicle skid intensity and gear state into the sound system */
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state == 3) continue;
        TD5_Actor *actor_snd = td5_game_get_actor(i);
        if (!actor_snd) continue;
        uint8_t *a = (uint8_t *)actor_snd;

        /* Skid intensity: max of front/rear axle slip excess (offset 0x31C, 0x320) */
        int slip_front = *(int32_t *)(a + 0x31C);
        int slip_rear  = *(int32_t *)(a + 0x320);
        int slip_max   = (slip_front > slip_rear) ? slip_front : slip_rear;
        if (slip_max < 0) slip_max = 0;

        /* Feed skid intensity for the viewport that is watching this vehicle */
        for (int vp = 0; vp < (g_td5.split_screen_mode ? 2 : 1); vp++) {
            if (g_actorSlotForView[vp] == i) {
                td5_sound_set_skid_intensity(vp, slip_max);
            }
        }

        /* Gear state (offset 0x224) -- used for horn volume table lookup */
        int gear = *(int32_t *)(a + 0x224);
        td5_sound_set_gear_state(i, gear);
    }

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_slot_state[i].state != 3) {
            td5_sound_update_vehicle_looping_state(i);
        }
    }
    td5_sound_update_audio_mix();
    td5_sound_tick();

    /* End scene and present */
    td5_render_end_scene();
    td5_plat_present(1);

    return 0;  /* race continues */
}

static void set_countdown_indicator_state(int value)
{
    int view_count = g_td5.viewport_count;

    if (view_count < 1) {
        view_count = 1;
    }
    if (view_count > 2) {
        view_count = 2;
    }

    for (int i = 0; i < view_count; i++) {
        td5_hud_set_indicator_state(i, value);
    }
}

static void reset_race_countdown(void)
{
    /* Original timer init: 0xA000 (160 ticks total, 40 per level).
     * Levels 4,3 → indicator 5,4 → blank atlas cells (not visible in original).
     * Only levels 2,1,0 → digits 3,2,1 are actually shown. */
    g_cameraTransitionActive = TD5_COUNTDOWN_INIT;
    s_race_countdown_ticks   = 0;
    s_race_countdown_state   = 0;   /* hide indicator until level 2 is reached */
    set_countdown_indicator_state(0);
    TD5_LOG_I(LOG_TAG, "Race countdown reset: timer=0x%X", g_cameraTransitionActive);
}

static void tick_race_countdown(void)
{
    int level, next_indicator;

    if (!g_td5.paused || g_cameraTransitionActive <= 0) {
        static int s_cd_early_logged = 0;
        if (s_cd_early_logged < 5) {
            TD5_LOG_W(LOG_TAG, "tick_countdown early return: paused=%d timer=%d",
                      g_td5.paused, g_cameraTransitionActive);
            s_cd_early_logged++;
        }
        return;
    }

    /* Decrement by 0x100 per sim tick — matches original UpdateRaceCameraTransitionTimer */
    g_cameraTransitionActive -= TD5_COUNTDOWN_DECR;

    if (g_cameraTransitionActive <= 0) {
        g_cameraTransitionActive = 0;
        set_countdown_indicator_state(0);
        g_td5.paused = 0;
        s_race_countdown_state = 0;
        TD5_LOG_I(LOG_TAG, "Race countdown complete: GO");
        return;
    }

    level = g_cameraTransitionActive / TD5_COUNTDOWN_LEVEL_DIV;
    /* Only levels 2,1,0 map to visible digits 3,2,1.
     * Levels 4,3 (indicator 5,4) correspond to blank atlas cells in the original
     * — don't show them to avoid rendering garbage sprites. */
    next_indicator = (level <= 2) ? (level + 1) : 0;
    if (next_indicator != s_race_countdown_state) {
        s_race_countdown_state = next_indicator;
        set_countdown_indicator_state(next_indicator);
        TD5_LOG_I(LOG_TAG, "Race countdown: level=%d indicator=%d timer=0x%X",
                  level, next_indicator, g_cameraTransitionActive);
    }
}

/* ========================================================================
 * Release race resources
 *
 * Called when fade-out completes. Releases sound, input, render resources.
 * ======================================================================== */

void td5_game_release_race_resources(void) {
    TD5_LOG_I(LOG_TAG, "Releasing race resources");

    /* Stop and release all race sound channels */
    td5_sound_release_race_channels();
    td5_sound_set_race_end(1);

    /* Stop force feedback and reset input config */
    td5_input_ff_stop();
    td5_input_ff_shutdown();

    /* Close input recording/playback */
    if (s_replay_mode) {
        td5_input_read_close();
    } else {
        td5_input_write_close();
    }
    td5_input_set_playback_active(0);

    /* Release render resources */
    td5_render_reset_texture_cache();

    /* Post-process: fix up any actors whose display position was unset */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        if (s_metrics[i].display_position < 0 ||
            s_metrics[i].display_position >= TD5_MAX_RACER_SLOTS) {
            s_metrics[i].display_position = (int16_t)i;
        }
    }
}

/* ========================================================================
 * Race Completion Detection
 *
 * Two-phase architecture matching CheckRaceCompletionState (0x409e80):
 *   Phase 1: Per-actor finish detection (when s_post_finish_cooldown == 0)
 *   Phase 2: Cooldown accumulator; when > 0x3FFFFF, build results table
 * ======================================================================== */

static int check_race_completion(uint32_t sim_delta) {
    int i;

    /* Phase 2: Post-finish cooldown */
    if (s_post_finish_cooldown != 0) {
        s_post_finish_cooldown += sim_delta;
        if (s_post_finish_cooldown > 0x3FFFFF) {
            /* Cooldown expired: build results and signal completion */
            TD5_LOG_I(LOG_TAG, "Race completion cooldown expired: building results");
            s_post_finish_cooldown = 0;
            build_results_table();
            return 1;
        }
        return 0;
    }

    /* Phase 1: Check if all required actors have finished */
    int all_finished = 1;

    if (g_td5.time_trial_enabled) {
        /* Time trial: only require slots 0 and 1 (human players) */
        for (i = 0; i < 2; i++) {
            if (s_slot_state[i].state != 3 &&
                s_metrics[i].post_finish_metric_base == 0) {
                all_finished = 0;
                break;
            }
        }
    } else {
        /* Normal race: require all active slots to finish */
        for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
            if (s_slot_state[i].state == 3) continue;  /* disabled */
            if (s_slot_state[i].companion_1 == 0) {     /* not finished */
                /* Exception: if slot 0 is done, allow spectator mode exit */
                if (i != 0 && s_metrics[0].post_finish_metric_base != 0) {
                    continue;  /* allow unfinished AI if player done */
                }
                all_finished = 0;
                break;
            }
        }
    }

    if (all_finished) {
        /* Latch the cooldown accumulator to begin phase 2 */
        s_post_finish_cooldown = 1;
        s_race_end_timer_start = td5_plat_time_ms();
        TD5_LOG_I(LOG_TAG,
                  "Race completion triggered: slot0=%d slot1=%d slot2=%d slot3=%d slot4=%d slot5=%d",
                  s_slot_state[0].companion_1,
                  s_slot_state[1].companion_1,
                  s_slot_state[2].companion_1,
                  s_slot_state[3].companion_1,
                  s_slot_state[4].companion_1,
                  s_slot_state[5].companion_1);
    }

    return 0;
}

/* ========================================================================
 * Advance pending finish state per actor (0x40A2B0)
 *
 * Per-tick update for each actor: check circuit sectors or P2P checkpoints.
 * ======================================================================== */

static void advance_pending_finish_state(int slot, uint32_t sim_delta) {
    ActorRaceMetric *m = &s_metrics[slot];

    /* Sync span from actor struct (original reads actor+0x82 directly) */
    {
        TD5_Actor *actor = td5_game_get_actor(slot);
        if (actor) {
            int16_t *track_state = (int16_t *)((uint8_t *)actor + 0x80);
            m->normalized_span = track_state[1];  /* actor+0x82: current span */
        }
    }

    /* Already finished */
    if (s_slot_state[slot].companion_1 != 0) return;

    /* Increment cumulative timer */
    m->cumulative_timer++;

    /* Circuit lap detection via 4-bit sector bitmask */
    if (g_td5.track_type == TD5_TRACK_CIRCUIT && !g_td5.time_trial_enabled) {
        int32_t span_ring = g_td5.track_span_ring_length;
        if (span_ring <= 0) return;

        int sector_size = span_ring / 4;
        for (int s = 0; s < 4; s++) {
            int sector_center = sector_size * s + sector_size / 2;
            int delta = m->normalized_span - sector_center;
            if (delta < 0) delta = -delta;
            if (delta <= 1) {
                /* Check strict ordering: bit s can only be set if bits 0..(s-1) are set */
                uint8_t expected_mask = (uint8_t)((1 << s) - 1);
                if ((m->checkpoint_bitmask & expected_mask) == expected_mask) {
                    m->checkpoint_bitmask |= (uint8_t)(1 << s);
                }
            }
        }

        /* All 4 sectors passed = lap complete */
        if (m->checkpoint_bitmask == 0x0F) {
            m->checkpoint_index++;
            m->checkpoint_bitmask = 0;

            /* Store lap split time */
            if (m->checkpoint_index > 0 && m->checkpoint_index <= 8) {
                m->lap_split_times[m->checkpoint_index - 1] =
                    (int16_t)m->cumulative_timer;
            }

            /* Check if race finished */
            if (m->checkpoint_index >= g_td5.circuit_lap_count) {
                m->post_finish_metric_base = m->cumulative_timer;
                s_slot_state[slot].companion_1 = 1;
                s_slot_state[slot].companion_2 = 1;
                s_slot_state[slot].state = 2;  /* completed */
                TD5_LOG_I(LOG_TAG,
                          "Actor finish: slot=%d mode=circuit lap=%d timer=%d span=%d",
                          slot, m->checkpoint_index, m->cumulative_timer, m->normalized_span);
            }
        }
    } else {
        /* Point-to-point / time trial: checkpoint crossing (0x409E80 P2P branch)
         * Original comparison: (int)(uint)(uint16_t)threshold <= (int)(int16_t)span */
        if (m->checkpoint_index < s_active_checkpoint.checkpoint_count) {
            int cp = m->checkpoint_index;
            int threshold = (int)(unsigned int)s_active_checkpoint.checkpoints[cp].span_threshold;
            int span_val  = (int)m->normalized_span;
            if (span_val >= threshold) {
                /* Crossed checkpoint: add time bonus */
                m->timer_ticks +=
                    (int16_t)s_active_checkpoint.checkpoints[cp].time_bonus;
                m->checkpoint_index++;
                TD5_LOG_I(LOG_TAG,
                          "Checkpoint crossed: slot=%d cp=%d span=%d threshold=%d bonus=%d timer=%d",
                          slot, cp, span_val, threshold,
                          (int)(int16_t)s_active_checkpoint.checkpoints[cp].time_bonus,
                          m->cumulative_timer);

                /* Store split time */
                if (cp < 8) {
                    m->lap_split_times[cp] = (int16_t)m->cumulative_timer;
                }
            }
        }

        /* Check if all checkpoints passed (skip if checkpoint data not loaded) */
        if (s_active_checkpoint.checkpoint_count > 0 &&
            m->checkpoint_index >= s_active_checkpoint.checkpoint_count) {
            m->post_finish_metric_base = m->cumulative_timer;
            if (m->average_speed_raw > 0) {
                int avg = (m->normalized_span * 1500) / m->average_speed_raw;
                m->speed_bonus += (avg * 1000 - m->average_speed_raw * 1000 / 256);
            }
            s_slot_state[slot].companion_1 = 1;
            s_slot_state[slot].companion_2 = 1;
            s_slot_state[slot].state = 2;
            TD5_LOG_I(LOG_TAG,
                      "Actor finish: slot=%d mode=checkpoint checkpoints=%d timer=%d span=%d",
                      slot, m->checkpoint_index, m->cumulative_timer, m->normalized_span);
        }
    }
}

/* ========================================================================
 * Accumulate speed bonus (0x40A3D0)
 * ======================================================================== */

static void accumulate_speed_bonus(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];

    /* Only accumulate for unfinished actors, every 4th tick */
    if (s_slot_state[slot].companion_1 != 0) return;
    if (m->forward_speed <= 0) return;
    if ((g_td5.simulation_tick_counter & 3) != 0) return;

    int bonus = (m->forward_speed >> 15) - (m->skid_factor >> 1);
    if (m->contact_count > 15 || bonus < 0) bonus = 0;

    /* Human player: no bonus if behind checkpoint */
    if (s_slot_state[slot].state == 1 && m->normalized_span < m->checkpoint_index) {
        bonus = 0;
    }

    m->accumulated_score += bonus;
}

/* ========================================================================
 * Decay ultimate variant timer (0x40A440)
 * ======================================================================== */

static void decay_ultimate_timer(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];

    if (s_slot_state[slot].companion_1 != 0) return;
    if (g_td5.race_rule_variant != 4) return;   /* Ultimate mode only */

    m->accumulated_score -= 1;
    if (m->accumulated_score < 0) m->accumulated_score = 0;
}

/* ========================================================================
 * Adjust checkpoint timers by difficulty (0x40A530)
 *
 * Applied once per human player during InitializeRaceSession.
 *   Easy   (tier 0): +20% time (multiply by 12/10)
 *   Normal (tier 1): +10% time (multiply by 11/10)
 *   Hard   (tier 2): no adjustment (baseline)
 * ======================================================================== */

static void adjust_checkpoint_timers(int slot) {
    ActorRaceMetric *m = &s_metrics[slot];
    int numerator = 10, denominator = 10;

    /* Set base timer from checkpoint record (0x40A530 writes to actor+0x344) */
    m->timer_ticks = (int16_t)s_active_checkpoint.initial_time;

    /* Clear checkpoint state (matches original: clears +0x37E, +0x328, +0x34C) */
    m->checkpoint_index = 0;
    m->post_finish_metric_base = 0;
    m->cumulative_timer = 0;

    switch (g_td5.difficulty) {
    case TD5_DIFFICULTY_EASY:   numerator = 12; break;  /* +20% */
    case TD5_DIFFICULTY_NORMAL: numerator = 11; break;  /* +10% */
    case TD5_DIFFICULTY_HARD:
    default:
        return;  /* hard = baseline, no scaling */
    }

    /* Scale initial time */
    m->timer_ticks = (int16_t)((int)s_active_checkpoint.initial_time *
                               numerator / denominator);

    /* Scale each checkpoint bonus */
    for (int i = 0; i < (int)s_active_checkpoint.checkpoint_count && i < 5; i++) {
        s_active_checkpoint.checkpoints[i].time_bonus =
            (uint16_t)(s_active_checkpoint.checkpoints[i].time_bonus *
                       numerator / denominator);
    }
}

/* ========================================================================
 * Build race results table (0x40A8C0)
 *
 * Populates s_results from s_metrics, then sorts by the appropriate
 * metric for the current game type.
 * ======================================================================== */

static void build_results_table(void) {
    int i;

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        ActorRaceMetric *m = &s_metrics[i];
        RaceResultEntry *r = &s_results[i];

        r->slot_flags = (s_slot_state[i].state != 3) ? 1 : 0;
        r->slot_index = (uint8_t)i;

        /* If AI didn't actually finish, synthesize a finish time */
        if (m->post_finish_metric_base == 0 && s_slot_state[i].state != 3) {
            int estimated = (rand() & 0x1F) + m->cumulative_timer + 100;
            m->post_finish_metric_base = estimated;
        }

        r->primary_metric   += m->post_finish_metric_base;
        r->secondary_metric += m->accumulated_score;
        r->wanted_kills      = (uint8_t)m->wanted_kills;
        r->speed_bonus       += m->speed_bonus;
        if (m->top_speed > r->top_speed) {
            r->top_speed = (int16_t)m->top_speed;
        }

        /* Award position points based on game type */
        int pos = s_race_order[i];
        if (g_td5.race_rule_variant == 0 && pos < TD5_MAX_RACER_SLOTS) {
            /* Championship: {15, 12, 10, 5, 4, 3} */
            r->secondary_metric += s_championship_points[pos];
        } else if (g_td5.race_rule_variant == 4 && pos < TD5_MAX_RACER_SLOTS) {
            /* Ultimate: {1000, 500, 250, 0, 0, 0} */
            r->secondary_metric += s_ultimate_points[pos];
        }
    }

    /* Sort results by the appropriate metric for the game type */
    switch (g_td5.game_type) {
    case TD5_GAMETYPE_CHAMPIONSHIP:
    case TD5_GAMETYPE_ULTIMATE:
        sort_results_by_score_desc();
        break;
    case TD5_GAMETYPE_ERA:
    case TD5_GAMETYPE_CHALLENGE:
    case TD5_GAMETYPE_PITBULL:
    case TD5_GAMETYPE_MASTERS:
    default:
        sort_results_by_time_asc();
        break;
    }
}

/* ========================================================================
 * Reset results table (0x40A880)
 * ======================================================================== */

static void reset_results_table(void) {
    memset(s_results, 0, sizeof(s_results));
    s_results[0].slot_flags = 1;  /* mark entry 0 as active */
}

/* ========================================================================
 * Sort results by primary metric ascending (fastest wins)
 * Bubble sort on s_race_order, matching SortRaceResultsByPrimaryMetricAsc
 * (0x40AAD0). Used for game types 2-5 (Era, Challenge, Pitbull, Masters).
 * ======================================================================== */

static void sort_results_by_time_asc(void) {
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];
            /* Compare: primary_metric * 100 / 30 ascending (lower = better) */
            int32_t val_a = s_results[a].primary_metric * 100 / 30;
            int32_t val_b = s_results[b].primary_metric * 100 / 30;
            if (val_a > val_b) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write final positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_results[s_race_order[i]].final_position = (int16_t)i;
    }
}

/* ========================================================================
 * Sort results by secondary metric descending (most points wins)
 * Bubble sort matching SortRaceResultsBySecondaryMetricDesc (0x40AB80).
 * Used for game types 1, 6 (Championship, Ultimate).
 * ======================================================================== */

static void sort_results_by_score_desc(void) {
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];
            if (s_results[a].secondary_metric < s_results[b].secondary_metric) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write final positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_results[s_race_order[i]].final_position = (int16_t)i;
    }
}

/* ========================================================================
 * Update race order (0x42F5B0)
 *
 * Bubble sort s_race_order by normalized span position (forward progress).
 * Called every sim tick inside RunRaceFrame.
 * ======================================================================== */

static void update_race_order(void) {
    int swapped;
    do {
        swapped = 0;
        for (int i = 0; i < TD5_MAX_RACER_SLOTS - 1; i++) {
            int a = s_race_order[i];
            int b = s_race_order[i + 1];

            /* Skip already-finished actors in span comparison */
            if (s_metrics[a].post_finish_metric_base != 0) continue;
            if (s_metrics[b].post_finish_metric_base != 0) continue;

            /* Higher span = further ahead = better position (lower index) */
            if (s_metrics[a].normalized_span < s_metrics[b].normalized_span) {
                s_race_order[i]     = (uint8_t)b;
                s_race_order[i + 1] = (uint8_t)a;
                swapped = 1;
            }
        }
    } while (swapped);

    /* Write display positions */
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_metrics[s_race_order[i]].display_position = (int16_t)i;
    }

    /* Time trial tiebreaker: if both slots 0 and 1 finished, compare by
     * finish_time * 256 - post_finish_metric for ms-level precision */
    if (g_td5.time_trial_enabled &&
        s_metrics[0].post_finish_metric_base != 0 &&
        s_metrics[1].post_finish_metric_base != 0) {
        int32_t t0 = s_metrics[0].post_finish_metric_base;
        int32_t t1 = s_metrics[1].post_finish_metric_base;
        if (t0 <= t1) {
            s_metrics[0].display_position = 0;
            s_metrics[1].display_position = 1;
        } else {
            s_metrics[0].display_position = 1;
            s_metrics[1].display_position = 0;
        }
    }
}

/* ========================================================================
 * Race Flow
 * ======================================================================== */

int td5_game_check_race_completion(void) {
    return check_race_completion(
        (uint32_t)(g_td5.normalized_frame_dt * 65536.0f));
}

/* ========================================================================
 * BeginRaceFadeOutTransition (0x42CC20)
 *
 * Sets fade state and selects direction based on viewport layout.
 *   Single player: alternates horizontal/vertical (s_fade_direction_alternator)
 *   Horizontal split: always direction 0 (horizontal)
 *   Vertical split:   always direction 1 (vertical)
 * ======================================================================== */

void td5_game_begin_fade_out(int param) {
    g_td5.race_end_fade_state = 1;
    s_fade_accumulator = 0.0f;

    switch (param) {
    case 0:  /* single player */
        g_td5.fade_direction = s_fade_direction_alternator;
        s_fade_direction_alternator ^= 1;  /* toggle for next race */
        break;
    case 1:  /* horizontal split */
        g_td5.fade_direction = 0;
        break;
    case 2:  /* vertical split */
        g_td5.fade_direction = 1;
        break;
    default:
        g_td5.fade_direction = 0;
        break;
    }

    TD5_LOG_I(LOG_TAG, "Fade out begin: param=%d direction=%d", param, g_td5.fade_direction);
}

static const char *td5_game_state_name(TD5_GameState state)
{
    switch (state) {
    case TD5_GAMESTATE_INTRO: return "INTRO";
    case TD5_GAMESTATE_MENU: return "MENU";
    case TD5_GAMESTATE_RACE: return "RACE";
    case TD5_GAMESTATE_BENCHMARK: return "BENCHMARK";
    default: return "UNKNOWN";
    }
}

/* ========================================================================
 * IsLocalRaceParticipantSlot (0x42CBE0)
 * ======================================================================== */

int td5_game_is_local_participant(int slot) {
    if (g_td5.network_active) return td5_net_is_slot_active(slot); /* dpu_exref[0xBCC + slot*4] */
    if (g_td5.split_screen_mode > 0) return (slot < 2);
    return (slot == 0);
}

/* ========================================================================
 * Frame Timing (from RunRaceFrame 0x42B580, timing block)
 *
 * g_frameEndTimestamp = td5_plat_time_ms()
 * frameDeltaMs = end - prev
 * g_instantFPS = 1000000.0 / frameDeltaMs
 * g_normalizedFrameDt = frameDeltaMs * timeScale
 * g_simTickBudget += smoothedDt (clamped to 4.0 max)
 * ======================================================================== */

void td5_game_update_frame_timing(void) {
    uint32_t now = td5_plat_time_ms();
    uint32_t delta_ms = now - g_td5.frame_prev_timestamp;

    /* Clamp minimum to avoid division by zero (and max to 100ms = 10fps) */
    if (delta_ms < 1) delta_ms = 1;
    if (delta_ms > 100) delta_ms = 100;

    /* Instant FPS */
    g_td5.instant_fps = 1000.0f / (float)delta_ms;

    /* Normalized frame delta time (in internal time units) */
    /* Original: frameDeltaMs * timeScale * tickScale */
    g_td5.normalized_frame_dt = (float)delta_ms / 1000.0f;

    /* Accumulate simulation budget (capped at 4.0 to prevent spiral of death) */
    g_td5.sim_tick_budget += g_td5.normalized_frame_dt;
    if (g_td5.sim_tick_budget > TD5_MAX_SIM_BUDGET) {
        g_td5.sim_tick_budget = TD5_MAX_SIM_BUDGET;
    }

    /* Convert budget to integer accumulator (0x10000 per tick at 30fps) */
    /* 1.0 normalized dt = 30 ticks worth of accumulator at 30fps game speed */
    g_td5.sim_time_accumulator += (uint32_t)(delta_ms * 0x10000 / 33);

    /* Benchmark mode: force constant sim budget for deterministic timing */
    if (g_td5.benchmark_active) {
        g_td5.sim_tick_budget = 3.0f;
    }

    g_td5.frame_end_timestamp = now;
    g_td5.frame_prev_timestamp = now;
}

float td5_game_get_fps(void) {
    return g_td5.instant_fps;
}

float td5_game_get_frame_dt(void) {
    return g_td5.normalized_frame_dt;
}

/* ========================================================================
 * Viewport Layout (0x42C2B0)
 *
 * 3 modes derived from render_width / render_height:
 *   Mode 0 (single):     1 viewport, full screen
 *   Mode 1 (horiz split): 2 viewports, top/bottom halves
 *   Mode 2 (vert split):  2 viewports, left/right halves
 * ======================================================================== */

void td5_game_init_viewport_layout(void) {
    int w = g_td5.render_width;
    int h = g_td5.render_height;

    switch (g_td5.split_screen_mode) {
    case 0: /* Single player -- fullscreen */
        g_td5.viewport_count = 1;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h;
        break;

    case 1: /* Horizontal split -- top/bottom */
        g_td5.viewport_count = 2;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h / 2;

        s_viewports[1].x = 0;
        s_viewports[1].y = h / 2;
        s_viewports[1].w = w;
        s_viewports[1].h = h / 2;
        break;

    case 2: /* Vertical split -- left/right */
        g_td5.viewport_count = 2;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w / 2;
        s_viewports[0].h = h;

        s_viewports[1].x = w / 2;
        s_viewports[1].y = 0;
        s_viewports[1].w = w / 2;
        s_viewports[1].h = h;
        break;

    default:
        g_td5.viewport_count = 1;
        s_viewports[0].x = 0;
        s_viewports[0].y = 0;
        s_viewports[0].w = w;
        s_viewports[0].h = h;
        break;
    }

    TD5_LOG_I(LOG_TAG, "Viewport layout: mode=%d, count=%d, %dx%d",
              g_td5.split_screen_mode, g_td5.viewport_count, w, h);
}

/* ========================================================================
 * Intro / Legal Screens
 * ======================================================================== */

void td5_game_play_intro_movie(void) {
    /* Original: PlayIntroMovie (0x43C440)
     * Plays Movie/intro.tgq via EA TGQ engine.
     * Source port: try MP4 first (transcoded from TGQ), then AVI, then WMV.
     * TGQ is not supported by Media Foundation -- it will be rejected early
     * with a log message telling the user to transcode. */
    if (td5_fmv_is_supported()) {
        if (!td5_fmv_play("Movie/intro.mp4") &&
            !td5_fmv_play("Movie/intro.avi") &&
            !td5_fmv_play("Movie/intro.wmv")) {
            /* None of the transcoded formats found. Try the original TGQ
             * path -- td5_fmv_play will log the "transcode to MP4" hint. */
            td5_fmv_play("Movie/intro.tgq");
        }
    }
}

void td5_game_show_legal_screens(void) {
    /* Original: ShowLegalScreens (0x42C8E0)
     * Loads legal1.tga, legal2.tga from LEGALS.ZIP and displays each for ~5s.
     * Source port: delegates to td5_fmv module which loads pre-extracted TGAs. */
    td5_fmv_show_legal_screens();
}

/* ========================================================================
 * Display Loading Screen TGA
 *
 * Selects a random loading image from LOADING.ZIP (load00.tga..load19.tga)
 * and displays it as a static screen while race session init completes.
 *
 * Image index = rand() % 20 (seeded from session seed chain).
 * ======================================================================== */

static void display_loading_screen_tga(void) {
    char png_path[128];
    int index = rand() % 20;
    void *pixels = NULL;
    int img_w = 0, img_h = 0;

    snprintf(png_path, sizeof(png_path), "../re/td5_png_clean/loading/load%02d.png", index);
    TD5_LOG_I(LOG_TAG, "Loading screen: %s", png_path);

    if (!td5_asset_load_png_to_buffer(png_path, TD5_COLORKEY_NONE, &pixels, &img_w, &img_h)) {
        TD5_LOG_W(LOG_TAG, "Loading screen %s not found", png_path);
        return;
    }

    /* Draw fullscreen quad and present */
    {
        int screen_w = 0, screen_h = 0;
        td5_plat_get_window_size(&screen_w, &screen_h);
        float sw = (float)screen_w;
        float sh = (float)screen_h;
        TD5_D3DVertex verts[4];
        uint16_t indices[6] = {0,1,2, 0,2,3};

        verts[0].screen_x = 0.0f; verts[0].screen_y = 0.0f;
        verts[0].depth_z = 0.0f;  verts[0].rhw = 1.0f;
        verts[0].diffuse = 0xFFFFFFFF; verts[0].specular = 0;
        verts[0].tex_u = 0.0f;    verts[0].tex_v = 0.0f;

        verts[1].screen_x = sw;   verts[1].screen_y = 0.0f;
        verts[1].depth_z = 0.0f;  verts[1].rhw = 1.0f;
        verts[1].diffuse = 0xFFFFFFFF; verts[1].specular = 0;
        verts[1].tex_u = 1.0f;    verts[1].tex_v = 0.0f;

        verts[2].screen_x = sw;   verts[2].screen_y = sh;
        verts[2].depth_z = 0.0f;  verts[2].rhw = 1.0f;
        verts[2].diffuse = 0xFFFFFFFF; verts[2].specular = 0;
        verts[2].tex_u = 1.0f;    verts[2].tex_v = 1.0f;

        verts[3].screen_x = 0.0f; verts[3].screen_y = sh;
        verts[3].depth_z = 0.0f;  verts[3].rhw = 1.0f;
        verts[3].diffuse = 0xFFFFFFFF; verts[3].specular = 0;
        verts[3].tex_u = 0.0f;    verts[3].tex_v = 1.0f;

        td5_plat_render_clear(0x00000000);
        td5_plat_render_upload_texture(0, pixels, img_w, img_h, 2);
        td5_plat_render_begin_scene();
        td5_plat_render_set_viewport(0, 0, screen_w, screen_h);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(0);
        td5_plat_render_draw_tris(verts, 4, indices, 6);
        td5_plat_render_end_scene();
        td5_plat_present(0);
        td5_plat_present_texture_page(0, 0);
        free(pixels);
    }
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

/** StoreRoundedVector3Ints (0x42CCD0)
 *  Converts 3 floats to rounded integers via the original __ftol behavior. */
void td5_game_store_rounded_vec3(const float *in, int32_t *out) {
    for (int i = 0; i < 3; i++) {
        out[i] = (int32_t)(in[i] + (in[i] >= 0.0f ? 0.5f : -0.5f));
    }
}

/* ========================================================================
 * Game Logic Helpers (migrated from td5re_stubs.c)
 * ======================================================================== */

int td5_game_get_player_slot(int viewport) {
    if (viewport < 0 || viewport > 1) return 0;
    return g_actorSlotForView[viewport];
}
int td5_game_is_split_screen(void) {
    return g_split_screen_mode != 0;
}
int td5_game_is_replay_active(void) { return 0; }
int td5_game_get_traffic_variant(int traffic_index) { (void)traffic_index; return 0; }
int td5_game_get_cop_actor_index(void) { return -1; }
int td5_game_is_wanted_mode(void) { return 0; }
void td5_game_advance_sky_rotation(void) { }

void *td5_game_heap_alloc(size_t size) {
    return calloc(1, size);
}
