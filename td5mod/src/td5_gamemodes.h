/**
 * td5_gamemodes.h - Custom Game Mode Extension Framework
 *
 * Hooks ConfigureGameTypeFlags and CheckRaceCompletionState to allow
 * INI-defined custom game modes with configurable rules, AI count,
 * rubber-band strength, and custom end conditions.
 *
 * Usage:
 *   1. Call GameModes_Init() from DllMain after MinHook is initialized.
 *   2. Define modes in [GameModes] section of td5_mod.ini.
 *   3. Modes are activated when g_selectedGameType matches a defined ModeN.Type.
 *
 * Custom end conditions:
 *   - TimeTrialMode: finish after N laps regardless of other racers
 *   - SurvivalMode: last car standing wins (eliminated by damage)
 *   - SpeedTrapMode: highest speed at checkpoint wins
 */

#ifndef TD5_GAMEMODES_H
#define TD5_GAMEMODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Custom Game Mode Rule Definitions
 * ======================================================================== */

/* End-condition mode flags (can be combined) */
#define ENDCOND_DEFAULT         0x00    /* Use vanilla CheckRaceCompletionState */
#define ENDCOND_TIME_TRIAL      0x01    /* Finish after N laps, ignore AI state */
#define ENDCOND_SURVIVAL        0x02    /* Last car standing (damage elimination) */
#define ENDCOND_SPEED_TRAP      0x04    /* Highest speed at checkpoint wins */

/* Weather override values */
#define WEATHER_RAIN            0
#define WEATHER_SNOW            1
#define WEATHER_CLEAR           2
#define WEATHER_DEFAULT         (-1)

/* Maximum number of custom game modes */
#define MAX_CUSTOM_MODES        16

typedef struct {
    int     active;                 /* 1 if this slot is defined in INI */
    int     game_type;              /* g_selectedGameType value to intercept (1-9) */
    char    name[64];               /* Display name for logging */

    /* --- Rule overrides --- */
    int     lap_count;              /* -1 = use vanilla, 0+ = override gCircuitLapCount */
    float   checkpoint_time_scale;  /* Multiplier for checkpoint time bonuses (1.0 = normal) */
    int     traffic_enabled;        /* -1 = default, 0 = force off, 1 = force on */
    int     encounters_enabled;     /* -1 = default, 0 = force off, 1 = force on */
    int     ai_count;               /* -1 = default (5), 1-5 = override racer count */
    float   rubber_band_strength;   /* -1.0 = default, 0.0-2.0 = scale factor */
    int     damage_enabled;         /* -1 = default, 0 = disable, 1 = enable */
    int     reverse_track;          /* -1 = default, 0 = force normal, 1 = force reverse */
    int     weather_override;       /* WEATHER_DEFAULT = no override, 0/1/2 = force */

    /* --- Custom end condition --- */
    int     end_condition;          /* Bitmask of ENDCOND_* flags */

    /* TimeTrialMode params */
    int     tt_lap_count;           /* Laps to complete before forced finish */

    /* SurvivalMode params */
    int     sv_damage_threshold;    /* Damage value that eliminates a racer (0 = any crash-out) */

    /* SpeedTrapMode params */
    int     st_checkpoint_index;    /* Which checkpoint triggers speed measurement (-1 = last) */

} TD5_CustomGameMode;

/* ========================================================================
 * Runtime State
 * ======================================================================== */

typedef struct {
    int     mode_active;            /* Non-zero if a custom mode is currently applied */
    int     mode_index;             /* Index into g_customModes[] */

    /* Survival tracking */
    int     sv_eliminated[6];       /* Per-slot elimination flag */
    int     sv_remaining;           /* Number of racers still alive */

    /* Speed trap tracking */
    int     st_speeds[6];           /* Per-slot max speed at trap checkpoint */
    int     st_triggered[6];        /* Per-slot: has passed the trap checkpoint */
    int     st_winner;              /* Slot index of current speed winner (-1 = none) */

} TD5_GameModeState;

/* ========================================================================
 * Public API
 * ======================================================================== */

/**
 * Initialize the game mode framework. Call after MH_Initialize().
 * Reads [GameModes] from INI, installs hooks on ConfigureGameTypeFlags
 * and CheckRaceCompletionState.
 *
 * @param ini_path  Full path to td5_mod.ini
 * @return          Number of custom modes loaded, or -1 on hook failure
 */
int GameModes_Init(const char *ini_path);

/**
 * Query whether a custom mode is currently active.
 * Safe to call from any hook during race state.
 */
int GameModes_IsActive(void);

/**
 * Get pointer to the currently active custom mode definition.
 * Returns NULL if no custom mode is active.
 */
const TD5_CustomGameMode *GameModes_GetCurrent(void);

/**
 * Get pointer to the runtime state (survival/speed-trap tracking).
 * Returns NULL if no custom mode is active.
 */
const TD5_GameModeState *GameModes_GetState(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_GAMEMODES_H */
