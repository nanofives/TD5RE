/**
 * td5_gamemodes.c - Custom Game Mode Extension Framework
 *
 * Hooks two key functions in the race pipeline:
 *   1. ConfigureGameTypeFlags (0x410CA0) - intercepts game type -> runtime flag mapping
 *   2. CheckRaceCompletionState (0x409E80) - injects custom end conditions
 *
 * Custom modes are defined in [GameModes] INI section and activated when
 * g_selectedGameType matches a configured type. All overrides are applied
 * on top of the vanilla flag setup, so unspecified fields use defaults.
 *
 * Requires: MinHook (MH_Initialize already called), td5_sdk.h globals.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "MinHook.h"
#include "td5_sdk.h"
#include "td5_gamemodes.h"

/* =========================================================================
 * Game addresses and typedefs
 * ========================================================================= */

/* ConfigureGameTypeFlags: undefined4 __cdecl ConfigureGameTypeFlags(void) */
typedef int (__cdecl *fn_ConfigureGameTypeFlags)(void);
static fn_ConfigureGameTypeFlags Original_ConfigureGameTypeFlags = NULL;
#define ADDR_ConfigureGameTypeFlags ((void*)0x00410CA0)

/* CheckRaceCompletionState: undefined4 __cdecl CheckRaceCompletionState(undefined *param_1) */
typedef int (__cdecl *fn_CheckRaceCompletionState_Ex)(void *param_1);
static fn_CheckRaceCompletionState_Ex Original_CheckRaceCompletionState = NULL;
#define ADDR_CheckRaceCompletionState ((void*)0x00409E80)

/* Additional game globals needed for custom modes */
#define g_raceRuleVariant       (*(int32_t*)0x004AAF74)
#define g_raceDifficultyTier    (*(int32_t*)0x004AACF0)  /* approx, set per-type */
#define g_raceWithinSeriesIndex (*(int32_t*)0x004972A8)
#define g_raceEndFadeState      (*(int32_t*)0x004AAEF8)
#define g_postFinishCooldown    (*(void**)0x00483980)     /* DAT_00483980 latch */
#define g_trackIsCircuit        (*(int32_t*)0x00466E94)

/* Actor damage accumulator (at offset +0x2CC within each actor slot) */
#define ACTOR_OFF_DAMAGE_ACCUM  0x2CC

/* Checkpoint progress byte (at offset +0x37E within each actor slot) */
#define ACTOR_OFF_CHECKPOINT_IDX 0x37E

/* Checkpoint timing record pointer */
#define g_checkpointRecord      (*(uint8_t**)0x004AED88)

/* Race slot state table */
#define g_raceSlotState         ((uint8_t*)0x004AADF4)
#define SLOT_STATE_STRIDE       4
#define SLOT_STATE_OFF          0   /* state byte */
#define SLOT_COMPANION_1_OFF    1   /* companion_state_1 (0=racing, 1=finished) */
#define SLOT_COMPANION_2_OFF    2   /* companion_state_2 (0=ok, 1=done, 2=DNF) */

/* BuildRaceResultsTable */
typedef void (__cdecl *fn_BuildRaceResultsTable)(void);
#define TD5_BuildRaceResultsTable ((fn_BuildRaceResultsTable)0x0040A8C0)

/* =========================================================================
 * Module state
 * ========================================================================= */

static TD5_CustomGameMode g_customModes[MAX_CUSTOM_MODES];
static int                g_numModes = 0;
static TD5_GameModeState  g_modeState;
static char               g_iniPath[MAX_PATH];

/* =========================================================================
 * INI helpers
 * ========================================================================= */

static int ReadINIInt(const char *section, const char *key, int def) {
    return GetPrivateProfileIntA(section, key, def, g_iniPath);
}

static float ReadINIFloat(const char *section, const char *key, float def) {
    char buf[32], defbuf[32];
    snprintf(defbuf, sizeof(defbuf), "%.4f", def);
    GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf), g_iniPath);
    return (float)atof(buf);
}

static void ReadINIString(const char *section, const char *key, const char *def,
                          char *out, int outsize) {
    GetPrivateProfileStringA(section, key, def, out, outsize, g_iniPath);
}

/* =========================================================================
 * Configuration loader
 * ========================================================================= */

static void LoadGameModes(void) {
    char section[32];
    int i;

    g_numModes = ReadINIInt("GameModes", "Count", 0);
    if (g_numModes > MAX_CUSTOM_MODES)
        g_numModes = MAX_CUSTOM_MODES;

    memset(g_customModes, 0, sizeof(g_customModes));

    for (i = 0; i < g_numModes; i++) {
        TD5_CustomGameMode *m = &g_customModes[i];
        snprintf(section, sizeof(section), "GameMode%d", i + 1);

        m->game_type = ReadINIInt(section, "Type", -1);
        if (m->game_type < 1 || m->game_type > 9)
            continue;

        m->active = 1;
        ReadINIString(section, "Name", "Custom", m->name, sizeof(m->name));

        /* Rule overrides (-1 = use vanilla default) */
        m->lap_count              = ReadINIInt(section, "LapCount", -1);
        m->checkpoint_time_scale  = ReadINIFloat(section, "CheckpointTimeScale", 1.0f);
        m->traffic_enabled        = ReadINIInt(section, "TrafficEnabled", -1);
        m->encounters_enabled     = ReadINIInt(section, "EncountersEnabled", -1);
        m->ai_count               = ReadINIInt(section, "AICount", -1);
        m->rubber_band_strength   = ReadINIFloat(section, "RubberBandStrength", -1.0f);
        m->damage_enabled         = ReadINIInt(section, "DamageEnabled", -1);
        m->reverse_track          = ReadINIInt(section, "ReverseTrack", -1);
        m->weather_override       = ReadINIInt(section, "WeatherOverride", WEATHER_DEFAULT);

        /* End conditions */
        m->end_condition = 0;
        if (ReadINIInt(section, "TimeTrialMode", 0))
            m->end_condition |= ENDCOND_TIME_TRIAL;
        if (ReadINIInt(section, "SurvivalMode", 0))
            m->end_condition |= ENDCOND_SURVIVAL;
        if (ReadINIInt(section, "SpeedTrapMode", 0))
            m->end_condition |= ENDCOND_SPEED_TRAP;

        /* End condition parameters */
        m->tt_lap_count        = ReadINIInt(section, "TTLapCount", 3);
        m->sv_damage_threshold = ReadINIInt(section, "SurvivalDamageThreshold", 0);
        m->st_checkpoint_index = ReadINIInt(section, "SpeedTrapCheckpoint", -1);

        /* Clamp AI count */
        if (m->ai_count > 5) m->ai_count = 5;
        if (m->ai_count >= 0 && m->ai_count < 1) m->ai_count = 1;
    }
}

/* =========================================================================
 * Mode matching
 * ========================================================================= */

/**
 * Find a custom mode that matches the current game type.
 * Returns mode index or -1 if no match.
 */
static int FindMatchingMode(void) {
    int i;
    for (i = 0; i < g_numModes; i++) {
        if (g_customModes[i].active && g_customModes[i].game_type == g_selectedGameType)
            return i;
    }
    return -1;
}

/* =========================================================================
 * Checkpoint time scale application
 *
 * Scales the time bonus values in the active checkpoint record.
 * Called once after ConfigureGameTypeFlags when a custom mode is active.
 * ========================================================================= */

static void ApplyCheckpointTimeScale(float scale) {
    uint8_t *rec;
    int count, i;

    if (scale == 1.0f)
        return;

    rec = g_checkpointRecord;
    if (!rec)
        return;

    count = rec[0];  /* checkpoint_count at +0x00 */
    if (count < 1 || count > 5)
        return;

    /* Scale initial time at +0x02 (uint16) */
    {
        uint16_t *init_time = (uint16_t *)(rec + 2);
        int scaled = (int)(*init_time * scale);
        if (scaled > 0xFFFF) scaled = 0xFFFF;
        if (scaled < 1) scaled = 1;
        *init_time = (uint16_t)scaled;
    }

    /* Scale each checkpoint bonus at +0x06 + i*4 (uint16) */
    for (i = 0; i < count; i++) {
        uint16_t *bonus = (uint16_t *)(rec + 6 + i * 4);
        int scaled = (int)(*bonus * scale);
        if (scaled > 0xFFFF) scaled = 0xFFFF;
        if (scaled < 0) scaled = 0;
        *bonus = (uint16_t)scaled;
    }
}

/* =========================================================================
 * Hook: ConfigureGameTypeFlags
 *
 * Calls the original to set up vanilla flags, then applies custom mode
 * overrides on top. This preserves all the original initialization logic
 * (results table reset, Masters permutation, NPC group lookup, etc.).
 * ========================================================================= */

static int __cdecl Hook_ConfigureGameTypeFlags(void) {
    int result;
    int mode_idx;
    TD5_CustomGameMode *m;

    /* Call original -- sets all vanilla flags */
    result = Original_ConfigureGameTypeFlags();

    /* Check for matching custom mode */
    mode_idx = FindMatchingMode();
    if (mode_idx < 0) {
        /* No custom mode -- clear state and return vanilla result */
        g_modeState.mode_active = 0;
        g_modeState.mode_index = -1;
        return result;
    }

    m = &g_customModes[mode_idx];
    g_modeState.mode_active = 1;
    g_modeState.mode_index = mode_idx;

    /* Reset runtime tracking */
    memset(g_modeState.sv_eliminated, 0, sizeof(g_modeState.sv_eliminated));
    g_modeState.sv_remaining = g_racerCount;
    memset(g_modeState.st_speeds, 0, sizeof(g_modeState.st_speeds));
    memset(g_modeState.st_triggered, 0, sizeof(g_modeState.st_triggered));
    g_modeState.st_winner = -1;

    /* --- Apply rule overrides --- */

    /* Lap count */
    if (m->lap_count >= 0) {
        g_circuitLapCount = m->lap_count;
    }

    /* Traffic */
    if (m->traffic_enabled == 0)
        g_trafficActorsEnabled = 0;
    else if (m->traffic_enabled == 1)
        g_trafficActorsEnabled = 1;

    /* Encounters */
    if (m->encounters_enabled == 0)
        g_specialEncounterEnabled = 0;
    else if (m->encounters_enabled == 1)
        g_specialEncounterEnabled = 1;

    /* AI count: adjust g_racerCount (DAT_004AAF00).
     * Slot 0 is always the human player. Slots 1-5 are AI.
     * We set racerCount = ai_count + 1 (1 human + N AI).
     * Also disable extra slots by setting their state to 0x03 (disabled). */
    if (m->ai_count >= 1 && m->ai_count <= 5) {
        int desired = m->ai_count + 1;  /* +1 for human player */
        int slot;

        if (desired < g_racerCount) {
            /* Disable excess slots */
            for (slot = desired; slot < 6; slot++) {
                uint8_t *state = g_raceSlotState + slot * SLOT_STATE_STRIDE;
                *state = 0x03;  /* disabled */
            }
        }
        g_racerCount = desired;
    }

    /* Damage enabled: disable by setting collision immunity flags.
     * gap_0376[3] >= 0x0F skips ResolveVehicleCollisionPair for that actor.
     * We set it to 0xFF to disable collision damage for all actors. */
    if (m->damage_enabled == 0) {
        int slot;
        for (slot = 0; slot < 6; slot++) {
            uint8_t *actor = g_actorRuntimeState + slot * ACTOR_STRIDE;
            actor[0x376 + 3] = 0xFF;   /* collision immunity */
            actor[0x376 + 6] = 0xFF;
        }
    }

    /* Reverse track */
    if (m->reverse_track == 0)
        g_reverseTrackDirection = 0;
    else if (m->reverse_track == 1)
        g_reverseTrackDirection = 1;

    /* Weather override */
    if (m->weather_override != WEATHER_DEFAULT) {
        g_weatherType = m->weather_override;
    }

    /* Checkpoint time scaling (apply after vanilla setup has loaded the record) */
    if (m->checkpoint_time_scale != 1.0f) {
        ApplyCheckpointTimeScale(m->checkpoint_time_scale);
    }

    /* Rubber band strength: stored as a scale factor applied via a separate
     * per-tick hook. We just store the value here; the actual scaling happens
     * in the CheckRaceCompletionState hook's per-tick logic or via a
     * dedicated ComputeAIRubberBandThrottle hook (see notes below). */

    return result;
}

/* =========================================================================
 * Survival mode: per-tick elimination check
 *
 * Checks each actor's damage accumulator against the threshold.
 * Eliminated actors have their state set to 0x02 (completed) with
 * companion_state_2 = 0x02 (DNF).
 * ========================================================================= */

static void SurvivalMode_CheckEliminations(void) {
    TD5_CustomGameMode *m = &g_customModes[g_modeState.mode_index];
    int slot;

    for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
        uint8_t *actor;
        uint8_t *state;
        int32_t damage;

        if (g_modeState.sv_eliminated[slot])
            continue;

        actor = g_actorRuntimeState + slot * ACTOR_STRIDE;
        damage = *(int32_t *)(actor + ACTOR_OFF_DAMAGE_ACCUM);

        /* Check threshold: if sv_damage_threshold == 0, use a high default
         * so any significant damage accumulation eliminates */
        if (m->sv_damage_threshold > 0) {
            if (damage < m->sv_damage_threshold)
                continue;
        } else {
            /* Default: eliminate when damage > 100000 (heavy collision) */
            if (damage < 100000)
                continue;
        }

        /* Eliminate this racer */
        g_modeState.sv_eliminated[slot] = 1;
        g_modeState.sv_remaining--;

        /* Set slot state to "completed" with DNF flag */
        state = g_raceSlotState + slot * SLOT_STATE_STRIDE;
        state[SLOT_STATE_OFF] = 0x02;       /* completed */
        state[SLOT_COMPANION_1_OFF] = 0x01; /* finished */
        state[SLOT_COMPANION_2_OFF] = 0x02; /* DNF/disqualified */
    }
}

/* =========================================================================
 * Speed trap mode: per-tick speed measurement
 *
 * Monitors each actor's checkpoint index. When an actor crosses the
 * designated trap checkpoint, records their forward speed.
 * ========================================================================= */

static void SpeedTrapMode_MeasureSpeeds(void) {
    TD5_CustomGameMode *m = &g_customModes[g_modeState.mode_index];
    int trap_cp;
    int slot;

    /* Determine which checkpoint is the trap */
    if (m->st_checkpoint_index >= 0) {
        trap_cp = m->st_checkpoint_index;
    } else {
        /* Default: last checkpoint before finish */
        uint8_t *rec = g_checkpointRecord;
        trap_cp = rec ? (rec[0] - 1) : 4;
        if (trap_cp < 0) trap_cp = 0;
    }

    for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
        uint8_t *actor;
        int cp_idx;
        int32_t speed;

        if (g_modeState.st_triggered[slot])
            continue;

        actor = g_actorRuntimeState + slot * ACTOR_STRIDE;
        cp_idx = actor[ACTOR_OFF_CHECKPOINT_IDX];

        /* Check if actor has passed the trap checkpoint */
        if (cp_idx <= trap_cp)
            continue;

        /* Record speed at the moment of crossing */
        speed = *(int32_t *)(actor + ACTOR_OFF_FORWARD_SPEED);
        if (speed < 0) speed = -speed;
        g_modeState.st_speeds[slot] = speed;
        g_modeState.st_triggered[slot] = 1;

        /* Track current winner */
        if (g_modeState.st_winner < 0 ||
            speed > g_modeState.st_speeds[g_modeState.st_winner]) {
            g_modeState.st_winner = slot;
        }
    }
}

/* =========================================================================
 * Hook: CheckRaceCompletionState
 *
 * For custom end conditions, wraps the vanilla check with additional logic.
 * The vanilla function handles per-actor checkpoint/lap progression and the
 * post-finish cooldown timer. We layer our custom checks on top.
 *
 * param_1 is the sim time delta (passed as undefined* but treated as int).
 * Returns 0 = continue racing, 1 = begin fade-out/results.
 * ========================================================================= */

static int __cdecl Hook_CheckRaceCompletionState(void *param_1) {
    int vanilla_result;
    TD5_CustomGameMode *m;

    /* If no custom mode is active, use vanilla behavior */
    if (!g_modeState.mode_active) {
        return Original_CheckRaceCompletionState(param_1);
    }

    m = &g_customModes[g_modeState.mode_index];

    /* If using default end condition, just delegate to vanilla */
    if (m->end_condition == ENDCOND_DEFAULT) {
        return Original_CheckRaceCompletionState(param_1);
    }

    /* Always call vanilla first -- it handles checkpoint progression,
     * lap counting, sector bitmask updates, and the cooldown timer.
     * We just override the finish detection logic. */
    vanilla_result = Original_CheckRaceCompletionState(param_1);

    /* --- Custom end condition checks --- */

    /* Time Trial Mode: player finishes after N laps regardless of AI */
    if (m->end_condition & ENDCOND_TIME_TRIAL) {
        uint8_t *player_actor = g_actorRuntimeState;
        uint8_t *player_state = g_raceSlotState;
        int player_checkpoint = player_actor[ACTOR_OFF_CHECKPOINT_IDX];

        /* In circuit mode, checkpoint_idx increments each lap.
         * Check if player has completed the required laps. */
        if (g_trackIsCircuit && player_checkpoint >= m->tt_lap_count) {
            /* Force player to finished state */
            if (player_state[SLOT_COMPANION_1_OFF] == 0) {
                player_state[SLOT_STATE_OFF] = 0x02;
                player_state[SLOT_COMPANION_1_OFF] = 0x01;
                player_state[SLOT_COMPANION_2_OFF] = 0x01;
            }

            /* Force all AI to finished state too (they'll get estimated times) */
            {
                int slot;
                for (slot = 1; slot < g_racerCount && slot < 6; slot++) {
                    uint8_t *state = g_raceSlotState + slot * SLOT_STATE_STRIDE;
                    if (state[SLOT_COMPANION_1_OFF] == 0) {
                        state[SLOT_STATE_OFF] = 0x02;
                        state[SLOT_COMPANION_1_OFF] = 0x01;
                        state[SLOT_COMPANION_2_OFF] = 0x01;
                    }
                }
            }

            /* Trigger cooldown if not already started */
            if (g_postFinishCooldown == NULL) {
                g_postFinishCooldown = (void *)1;
            }
        }
    }

    /* Survival Mode: eliminate racers by damage, last standing wins */
    if (m->end_condition & ENDCOND_SURVIVAL) {
        SurvivalMode_CheckEliminations();

        /* Race ends when only 1 racer remains, or player is eliminated */
        if (g_modeState.sv_remaining <= 1 || g_modeState.sv_eliminated[0]) {
            /* Mark all non-eliminated as finished */
            int slot;
            for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
                uint8_t *state = g_raceSlotState + slot * SLOT_STATE_STRIDE;
                if (!g_modeState.sv_eliminated[slot] &&
                    state[SLOT_COMPANION_1_OFF] == 0) {
                    state[SLOT_STATE_OFF] = 0x02;
                    state[SLOT_COMPANION_1_OFF] = 0x01;
                    state[SLOT_COMPANION_2_OFF] = 0x01;
                }
            }

            if (g_postFinishCooldown == NULL) {
                g_postFinishCooldown = (void *)1;
            }
        }
    }

    /* Speed Trap Mode: measure speeds, end race when all have passed trap */
    if (m->end_condition & ENDCOND_SPEED_TRAP) {
        int all_triggered = 1;
        int slot;

        SpeedTrapMode_MeasureSpeeds();

        /* Check if all racers have passed the trap */
        for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
            if (!g_modeState.st_triggered[slot]) {
                all_triggered = 0;
                break;
            }
        }

        if (all_triggered) {
            /* Assign positions based on speed (highest = 1st) */
            /* Force all to finished */
            for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
                uint8_t *state = g_raceSlotState + slot * SLOT_STATE_STRIDE;
                if (state[SLOT_COMPANION_1_OFF] == 0) {
                    state[SLOT_STATE_OFF] = 0x02;
                    state[SLOT_COMPANION_1_OFF] = 0x01;
                    state[SLOT_COMPANION_2_OFF] = 0x01;
                }
            }

            /* Override display positions based on speed ranking.
             * gap_0376[0xD] is the display position (0=1st, 5=6th). */
            {
                int ranked[6] = {0, 1, 2, 3, 4, 5};
                int j;
                /* Simple sort by speed descending */
                for (slot = 0; slot < g_racerCount - 1; slot++) {
                    for (j = slot + 1; j < g_racerCount && j < 6; j++) {
                        if (g_modeState.st_speeds[ranked[j]] >
                            g_modeState.st_speeds[ranked[slot]]) {
                            int tmp = ranked[slot];
                            ranked[slot] = ranked[j];
                            ranked[j] = tmp;
                        }
                    }
                }
                for (slot = 0; slot < g_racerCount && slot < 6; slot++) {
                    uint8_t *actor = g_actorRuntimeState + ranked[slot] * ACTOR_STRIDE;
                    actor[0x376 + 0xD] = (uint8_t)slot;  /* position */
                }
            }

            if (g_postFinishCooldown == NULL) {
                g_postFinishCooldown = (void *)1;
            }
        }
    }

    return vanilla_result;
}

/* =========================================================================
 * Rubber band strength scaling
 *
 * The AI rubber-band is computed in ComputeAIRubberBandThrottle (0x432D60),
 * called from UpdateRaceActors. For simplicity, we apply the scale factor
 * by hooking the result globals rather than the function itself.
 *
 * The rubber-band throttle multiplier is written per-AI-slot in the
 * gActorRouteStateTable area. A strength of 0.0 effectively disables it,
 * while 2.0 doubles it. This is applied via the NoOpHookStub lifecycle
 * hook -- the caller (td5_mod.c) should call GameModes_ApplyRubberBandScale
 * from their existing NoOpHookStub hook.
 * ========================================================================= */

/**
 * Apply rubber band scaling. Call this from a per-frame hook (e.g. NoOpHookStub)
 * during race state (g_gameState == 2).
 */
void GameModes_ApplyRubberBandScale(void) {
    /* Rubber band globals at DAT_004AFB84 area, stride 0x47*4 per slot.
     * The actual throttle scaling is done in ComputeAIRubberBandThrottle
     * and written to the AI throttle command. We hook the output. */
    /* TODO: Hook ComputeAIRubberBandThrottle (0x432D60) with MinHook
     * for precise rubber-band scaling. For now, this is a placeholder
     * that documents the approach. The AI throttle override lives at
     * actor offset +0x33E (int16 throttle command) and can be scaled
     * post-UpdateRaceActors. */
    (void)0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int GameModes_IsActive(void) {
    return g_modeState.mode_active;
}

const TD5_CustomGameMode *GameModes_GetCurrent(void) {
    if (!g_modeState.mode_active || g_modeState.mode_index < 0)
        return NULL;
    return &g_customModes[g_modeState.mode_index];
}

const TD5_GameModeState *GameModes_GetState(void) {
    if (!g_modeState.mode_active)
        return NULL;
    return &g_modeState;
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

int GameModes_Init(const char *ini_path) {
    MH_STATUS status;
    int count = 0;
    int i;

    strncpy(g_iniPath, ini_path, MAX_PATH - 1);
    g_iniPath[MAX_PATH - 1] = '\0';

    /* Reset state */
    memset(&g_modeState, 0, sizeof(g_modeState));
    g_modeState.mode_index = -1;
    g_modeState.st_winner = -1;

    /* Load mode definitions from INI */
    LoadGameModes();

    /* Count active modes */
    for (i = 0; i < g_numModes; i++) {
        if (g_customModes[i].active)
            count++;
    }

    if (count == 0)
        return 0;  /* No modes defined, no hooks needed */

    /* Hook ConfigureGameTypeFlags */
    status = MH_CreateHook(ADDR_ConfigureGameTypeFlags,
                           (void *)Hook_ConfigureGameTypeFlags,
                           (void **)&Original_ConfigureGameTypeFlags);
    if (status != MH_OK)
        return -1;

    status = MH_EnableHook(ADDR_ConfigureGameTypeFlags);
    if (status != MH_OK)
        return -1;

    /* Hook CheckRaceCompletionState */
    status = MH_CreateHook(ADDR_CheckRaceCompletionState,
                           (void *)Hook_CheckRaceCompletionState,
                           (void **)&Original_CheckRaceCompletionState);
    if (status != MH_OK)
        return -1;

    status = MH_EnableHook(ADDR_CheckRaceCompletionState);
    if (status != MH_OK)
        return -1;

    return count;
}
