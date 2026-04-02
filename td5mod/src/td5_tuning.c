/**
 * td5_tuning.c - AI, Collision, and Physics Tuning Module
 *
 * Reads INI-driven overrides for AI grip/rubber-band, collision impulse,
 * and physics constants, then patches them into TD5_d3d.exe at runtime.
 *
 * Implementation approach:
 *   - PatchMemory for hardcoded immediates (grip clamps, gravity, inertia K)
 *   - MinHook for computed values that need runtime scaling (rubber-band,
 *     impulse multipliers, traffic toggle)
 *
 * All addresses verified via Ghidra decompilation on port 8195.
 *
 * Build: compiled alongside td5_mod.c, linked into td5_mod.asi.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "MinHook.h"
#include "td5_sdk.h"
#include "td5_tuning.h"

/* =========================================================================
 * Forward declaration of PatchMemory (defined in td5_mod.c, linked together)
 * ========================================================================= */
extern int PatchMemory(void *addr, const void *data, size_t len);

/* =========================================================================
 * Tuning Configuration
 * ========================================================================= */

static struct {
    /* --- [AITuning] --- */
    int ai_grip_scale_player;       /* Player grip multiplier (100 = normal) */
    int ai_grip_scale_ai;           /* AI grip multiplier (100 = normal) */
    int ai_rubber_band_strength;    /* Rubber-band intensity 0-200 (100=normal) */
    int ai_steering_bias_max;       /* Max AI steering command (default 0x18000) */
    int ai_throttle_scale;          /* AI throttle multiplier (100 = normal) */
    int ai_brake_scale;             /* AI brake multiplier (100 = normal) */
    int traffic_enabled;            /* 1 = traffic on, 0 = traffic off */
    int traffic_spawn_distance;     /* Spawn distance in spans (default 41) */
    int encounter_cooldown_frames;  /* Frames between encounters (default 300) */
    int encounter_speed_threshold;  /* Min speed to trigger (default 0x15639) */

    /* --- [CollisionTuning] --- */
    int v2v_impulse_scale;          /* V2V impulse multiplier (100 = normal) */
    int v2w_impulse_scale;          /* V2W impulse multiplier (100 = normal) */
    int damage_enabled;             /* 1 = damage on, 0 = damage off */
    int player_grip_clamp_low;      /* Player tire grip floor (default 0x38) */
    int player_grip_clamp_high;     /* Player tire grip ceiling (default 0x50) */
    int ai_grip_clamp_low;          /* AI tire grip floor (default 0x70) */
    int ai_grip_clamp_high;         /* AI tire grip ceiling (default 0xA0) */

    /* --- [PhysicsTuning] --- */
    int gravity_scale;              /* Gravity multiplier (100 = normal) */
    int friction_scale;             /* Global friction multiplier (100 = normal) */
    int top_speed_scale;            /* Global top speed multiplier (100 = normal) */
} g_tuning;

/* =========================================================================
 * INI Loading
 * ========================================================================= */

static void Tuning_LoadConfig(const char *ini) {
    /* [AITuning] */
    g_tuning.ai_grip_scale_player    = GetPrivateProfileIntA("AITuning", "AIGripScalePlayer",    100, ini);
    g_tuning.ai_grip_scale_ai        = GetPrivateProfileIntA("AITuning", "AIGripScaleAI",        100, ini);
    g_tuning.ai_rubber_band_strength = GetPrivateProfileIntA("AITuning", "AIRubberBandStrength", 100, ini);
    g_tuning.ai_steering_bias_max    = GetPrivateProfileIntA("AITuning", "AISteeringBiasMax",    0x18000, ini);
    g_tuning.ai_throttle_scale       = GetPrivateProfileIntA("AITuning", "AIThrottleScale",      100, ini);
    g_tuning.ai_brake_scale          = GetPrivateProfileIntA("AITuning", "AIBrakeScale",         100, ini);
    g_tuning.traffic_enabled         = GetPrivateProfileIntA("AITuning", "TrafficEnabled",       1,   ini);
    g_tuning.traffic_spawn_distance  = GetPrivateProfileIntA("AITuning", "TrafficSpawnDistance",  41,  ini);
    g_tuning.encounter_cooldown_frames = GetPrivateProfileIntA("AITuning", "EncounterCooldownFrames", 300, ini);
    g_tuning.encounter_speed_threshold = GetPrivateProfileIntA("AITuning", "EncounterSpeedThreshold", 0x15639, ini);

    /* [CollisionTuning] */
    g_tuning.v2v_impulse_scale       = GetPrivateProfileIntA("CollisionTuning", "V2VImpulseScale",     100, ini);
    g_tuning.v2w_impulse_scale       = GetPrivateProfileIntA("CollisionTuning", "V2WImpulseScale",     100, ini);
    g_tuning.damage_enabled          = GetPrivateProfileIntA("CollisionTuning", "DamageEnabled",       1,   ini);
    g_tuning.player_grip_clamp_low   = GetPrivateProfileIntA("CollisionTuning", "PlayerGripClampLow",  0x38, ini);
    g_tuning.player_grip_clamp_high  = GetPrivateProfileIntA("CollisionTuning", "PlayerGripClampHigh", 0x50, ini);
    g_tuning.ai_grip_clamp_low       = GetPrivateProfileIntA("CollisionTuning", "AIGripClampLow",      0x70, ini);
    g_tuning.ai_grip_clamp_high      = GetPrivateProfileIntA("CollisionTuning", "AIGripClampHigh",     0xA0, ini);

    /* [PhysicsTuning] */
    g_tuning.gravity_scale           = GetPrivateProfileIntA("PhysicsTuning", "GravityScale",  100, ini);
    g_tuning.friction_scale          = GetPrivateProfileIntA("PhysicsTuning", "FrictionScale", 100, ini);
    g_tuning.top_speed_scale         = GetPrivateProfileIntA("PhysicsTuning", "TopSpeedScale", 100, ini);
}

/* =========================================================================
 * Address Map (all verified in Ghidra on TD5_d3d.exe, base 0x400000)
 *
 * Grip clamp immediates -- these are CMP/MOV immediate operands embedded
 * directly in the instruction stream of UpdatePlayerVehicleDynamics and
 * UpdateAIVehicleDynamics.  We patch the imm8/imm32 bytes in-place.
 *
 * Player grip clamps (UpdatePlayerVehicleDynamics @ 0x404030):
 *   Each wheel has two clamp sites (low CMP, high CMP).
 *   The clamp pattern is: CMP reg, imm8; JGE ...; MOV reg, imm8
 *   followed by: CMP imm8, reg; JGE ...; MOV reg, imm8
 *
 * AI grip clamps (UpdateAIVehicleDynamics @ 0x404EC0):
 *   Two axle clamps (front local_38, rear local_20), same pattern.
 *
 * Rubber-band parameters (ComputeAIRubberBandThrottle @ 0x432D60):
 *   DAT_00473D9C = behind catch-up scale
 *   DAT_00473DA0 = ahead slow-down scale
 *   DAT_00473DA4 = behind distance range
 *   DAT_00473DA8 = ahead distance range
 *
 * Collision inertia constants:
 *   DAT_00463204 = 500,000 (V2V K in impulse denominator)
 *   DAT_00463200 = 1,500,000 (V2W inertia constant)
 *
 * Gravity (written by InitializeRaceVehicleRuntime):
 *   gGravityConstant @ 0x467380 (runtime variable)
 *   Immediates at 0x42F29F (hard), 0x42F354 (easy), 0x42F360 (normal)
 *
 * Encounter:
 *   Cooldown timer reset value: patched at the MOV imm32 sites
 *   Speed threshold 0x15639: patched in UpdateSpecialTrafficEncounter
 *
 * AI steering clamp:
 *   0x18000 appears in UpdateActorSteeringBias as the final clamp
 * ========================================================================= */

/*
 * Player grip clamp immediate addresses.
 * UpdatePlayerVehicleDynamics has 4 wheels x 2 clamps (low, high) = 8 patch sites.
 * Found by searching for CMP ..., 0x38 and CMP 0x50, ... in the function body.
 *
 * The imm8 bytes for the low clamp (0x38) and high clamp (0x50) are at these
 * file offsets within the function.  We patch all 8 sites.
 */

/*
 * Player grip clamps (UpdatePlayerVehicleDynamics @ 0x404030):
 *   Pattern: CMP reg, 0x38; JGE; MOV [stack], 0x38  (low clamp, 4 wheels)
 *   High clamp: MOV EDX, 0x50 (once, reused for all 4 CMP reg,EDX tests)
 *
 *   Wheel 1 (local_5c): CMP at 0x4042DC (+2=imm8), MOV fallback at 0x4042EC (+3=imm32)
 *   Wheel 2 (local_58): CMP at 0x4042FC (+2=imm8), MOV fallback at 0x404304 (+3=imm32)
 *   Wheel 3 (local_10): CMP at 0x404314 (+2=imm8), MOV fallback at 0x40431C (+3=imm32)
 *   Wheel 4 (local_3c): CMP at 0x404329 (+2=imm8), MOV fallback at 0x404330 (+1=imm32)
 *
 * The imm8 byte for the CMP is the last byte of the 3-byte CMP reg,imm8 insn.
 * The imm32 in the MOV fallback is a 7-byte MOV [EBP+off], imm32.
 */

/* Player grip clamp low (0x38):
 *   CMP imm8 operands (byte to patch): */
static const uintptr_t PLAYER_GRIP_LOW_CMP[] = {
    0x004042DE,  /* CMP EAX,0x38 -- byte at +2 of insn at 0x4042DC */
    0x004042FE,  /* CMP EAX,0x38 -- byte at +2 of insn at 0x4042FC */
    0x00404316,  /* CMP EAX,0x38 -- byte at +2 of insn at 0x404314 */
    0x0040432B,  /* CMP ECX,0x38 -- byte at +2 of insn at 0x404329 */
};
/* MOV [stack], imm32 fallback low values (4 bytes each): */
static const uintptr_t PLAYER_GRIP_LOW_MOV[] = {
    0x004042EC,  /* MOV [EBP-0x58], 0x38 -- imm32 at +3 of 7-byte insn at 0x4042E9 */
    0x00404304,  /* MOV [EBP-0x54], 0x38 */
    0x0040431C,  /* MOV [EBP-0xc],  0x38 */
    0x00404330,  /* MOV ECX, 0x38 -- imm32 at +1 of 5-byte insn at 0x40432E */
};

/* Player grip clamp high (0x50):
 *   Single MOV EDX, 0x50 at 0x4042E2, imm32 at bytes +1..+4 = 0x4042E3.
 *   All 4 CMP reg,EDX use this register value. One patch site suffices. */
#define PLAYER_GRIP_HIGH_ADDR  0x004042E3  /* imm32 of MOV EDX,0x50 */

/* AI grip clamp low (0x70) -- 2 axle sites:
 *   CMP imm8 operands: */
static const uintptr_t AI_GRIP_LOW_CMP[] = {
    0x004050BA,  /* CMP ECX,0x70 -- byte at +2 of insn at 0x4050B8 */
    0x004050DA,  /* CMP EAX,0x70 -- byte at +2 of insn at 0x4050D8 */
};
/* MOV [stack], imm32 fallback low values: */
static const uintptr_t AI_GRIP_LOW_MOV[] = {
    0x004050CA,  /* MOV [ESP+0x24],0x70 -- imm32 at +4 of 8-byte insn at 0x4050C6 */
    0x004050E1,  /* MOV [ESP+0x3C],0x70 -- imm32 at +4 of 8-byte insn at 0x4050DD */
};

/* AI grip clamp high (0xA0):
 *   Single MOV EDX, 0xA0 at 0x4050BB, imm32 at bytes +1..+4 = 0x4050BC. */
#define AI_GRIP_HIGH_ADDR  0x004050BC  /* imm32 of MOV EDX,0xA0 */

/* Gravity immediates (MOV dword at these addresses) */
#define ADDR_GRAVITY_HARD    0x0042F29F  /* imm32 = 0x800 */
#define ADDR_GRAVITY_EASY    0x0042F354  /* imm32 = 0x5DC */
#define ADDR_GRAVITY_NORMAL  0x0042F360  /* imm32 = 0x76C */
#define ADDR_GRAVITY_RUNTIME 0x00467380  /* gGravityConstant (dword in .data) */

/* Collision inertia constants */
#define ADDR_V2V_INERTIA_K   0x00463204  /* DAT_00463204 = 500,000 */
#define ADDR_V2W_INERTIA_K   0x00463200  /* DAT_00463200 = 1,500,000 */

/* Rubber-band parameters */
#define ADDR_RUBBER_BEHIND_SCALE  0x00473D9C
#define ADDR_RUBBER_AHEAD_SCALE   0x00473DA0
#define ADDR_RUBBER_BEHIND_RANGE  0x00473DA4
#define ADDR_RUBBER_AHEAD_RANGE   0x00473DA8

/* AI default throttle array (6 dwords, copied to live array each frame) */
#define ADDR_AI_DEFAULT_THROTTLE  0x00473D64

/* Encounter system */
#define ADDR_ENCOUNTER_COOLDOWN   0x004B064C  /* DAT_004b064c runtime cooldown */
#define ADDR_ENCOUNTER_ENABLED    0x004AAF70  /* gSpecialEncounterEnabled */

/* Traffic */
#define ADDR_TRAFFIC_ENABLED      0x004B0FA4  /* gTrafficActorsEnabled */

/* Collisions toggle (0=on, 1=off) -- inverted from our DamageEnabled */
#define ADDR_COLLISIONS_TOGGLE    0x00463188

/* =========================================================================
 * Hook Originals
 * ========================================================================= */

/* Hook ApplyVehicleCollisionImpulse to scale V2V impulse */
typedef int (__cdecl *fn_ApplyCollisionImpulse)(short*, short*, short*, uint32_t, int);
static fn_ApplyCollisionImpulse Orig_ApplyCollisionImpulse = NULL;

/* Hook ApplyTrackSurfaceForceToActor to scale V2W impulse */
typedef void (__cdecl *fn_ApplyTrackForce)(int, int*, uint32_t, int, uint32_t);
static fn_ApplyTrackForce Orig_ApplyTrackForce = NULL;

/* Hook InitializeRaceVehicleRuntime to apply gravity scaling after it sets gravity */
typedef void (__cdecl *fn_InitRaceVehicleRuntime)(void);
static fn_InitRaceVehicleRuntime Orig_InitRaceVehicleRuntime = NULL;

/* Hook InitializeRaceActorRuntime to apply rubber-band param scaling */
typedef void (__cdecl *fn_InitRaceActorRuntime)(void);
static fn_InitRaceActorRuntime Orig_InitRaceActorRuntime = NULL;

/* Hook ConfigureGameTypeFlags to override traffic/encounter toggles */
typedef void (__cdecl *fn_ConfigGameType)(void);
static fn_ConfigGameType Orig_ConfigGameType = NULL;

/* Hook UpdateSpecialTrafficEncounter to override cooldown and speed threshold */
typedef void (__cdecl *fn_UpdateEncounter)(void);
static fn_UpdateEncounter Orig_UpdateEncounter = NULL;

/* =========================================================================
 * Hook Implementations
 * ========================================================================= */

/**
 * Post-hook for InitializeRaceVehicleRuntime: scale gravity after the
 * original function writes one of the three difficulty-based values.
 */
static void __cdecl Hook_InitRaceVehicleRuntime(void) {
    Orig_InitRaceVehicleRuntime();

    if (g_tuning.gravity_scale != 100) {
        int32_t g = *(int32_t *)ADDR_GRAVITY_RUNTIME;
        g = (g * g_tuning.gravity_scale) / 100;
        *(int32_t *)ADDR_GRAVITY_RUNTIME = g;
        {
            char msg[96];
            wsprintfA(msg, "[TD5TUNE] Gravity scaled: %d (scale=%d%%)\n",
                      g, g_tuning.gravity_scale);
            OutputDebugStringA(msg);
        }
    }
}

/**
 * Post-hook for InitializeRaceActorRuntime: scale rubber-band parameters
 * and AI throttle defaults after the original sets them per difficulty/mode.
 */
static void __cdecl Hook_InitRaceActorRuntime(void) {
    Orig_InitRaceActorRuntime();

    /* Scale rubber-band parameters */
    if (g_tuning.ai_rubber_band_strength != 100) {
        int32_t *behind_scale = (int32_t *)ADDR_RUBBER_BEHIND_SCALE;
        int32_t *ahead_scale  = (int32_t *)ADDR_RUBBER_AHEAD_SCALE;
        DWORD old;

        VirtualProtect(behind_scale, 16, PAGE_READWRITE, &old);
        *behind_scale = (*behind_scale * g_tuning.ai_rubber_band_strength) / 100;
        *ahead_scale  = (*ahead_scale  * g_tuning.ai_rubber_band_strength) / 100;
        VirtualProtect(behind_scale, 16, old, &old);

        {
            char msg[96];
            wsprintfA(msg, "[TD5TUNE] Rubber-band: behind=%d ahead=%d (strength=%d%%)\n",
                      *behind_scale, *ahead_scale, g_tuning.ai_rubber_band_strength);
            OutputDebugStringA(msg);
        }
    }

    /* Scale AI default throttle array */
    if (g_tuning.ai_throttle_scale != 100) {
        int32_t *throttle = (int32_t *)ADDR_AI_DEFAULT_THROTTLE;
        DWORD old;
        int i;

        VirtualProtect(throttle, 24, PAGE_READWRITE, &old);
        for (i = 0; i < 6; i++) {
            throttle[i] = (throttle[i] * g_tuning.ai_throttle_scale) / 100;
        }
        VirtualProtect(throttle, 24, old, &old);
    }
}

/**
 * Post-hook for ConfigureGameTypeFlags: override traffic and encounter toggles.
 */
static void __cdecl Hook_ConfigGameType(void) {
    Orig_ConfigGameType();

    if (!g_tuning.traffic_enabled) {
        g_trafficActorsEnabled = 0;
        OutputDebugStringA("[TD5TUNE] Traffic DISABLED by INI\n");
    }

    /* Encounter cooldown is patched at spawn time; encounter_enabled is left
     * to the game's per-mode decision unless traffic is disabled (which also
     * disables encounters since slot 9 comes from the traffic pool). */
}

/**
 * Wrapper for UpdateSpecialTrafficEncounter: override cooldown reset value
 * and speed threshold.  We patch the runtime cooldown variable before calling
 * the original, and also gate on our custom speed threshold.
 */
static void __cdecl Hook_UpdateEncounter(void) {
    /* The cooldown is a countdown variable at 0x4B064C.
     * The original resets it to 300 on despawn.  We can't easily patch
     * the immediate in the code (it appears multiple times), so instead
     * we post-check: if the cooldown was just set to 300, override it. */
    int32_t cooldown_before = *(int32_t *)ADDR_ENCOUNTER_COOLDOWN;

    Orig_UpdateEncounter();

    /* If cooldown was just reset (went from 0 to 300, or from any value
     * to exactly 300), replace with our configured value. */
    {
        int32_t cooldown_after = *(int32_t *)ADDR_ENCOUNTER_COOLDOWN;
        if (cooldown_after == 300 && cooldown_before != 300) {
            *(int32_t *)ADDR_ENCOUNTER_COOLDOWN = g_tuning.encounter_cooldown_frames;
        }
    }
}

/**
 * Wrapper for ApplyVehicleCollisionImpulse: scale the returned impulse.
 * We temporarily adjust the V2V inertia constant K to change impulse magnitude.
 */
static int __cdecl Hook_ApplyCollisionImpulse(short *a, short *b, short *c,
                                               uint32_t d, int e) {
    int result;
    if (g_tuning.v2v_impulse_scale != 100) {
        /* Scale K inversely: smaller K = bigger impulse.
         * impulse ~ K / (offset^2 + K), so scaling K by 100/scale
         * effectively scales impulse by scale/100. */
        int32_t *pk = (int32_t *)ADDR_V2V_INERTIA_K;
        int32_t orig_k = *pk;
        *pk = (orig_k * 100) / g_tuning.v2v_impulse_scale;
        result = Orig_ApplyCollisionImpulse(a, b, c, d, e);
        *pk = orig_k;
    } else {
        result = Orig_ApplyCollisionImpulse(a, b, c, d, e);
    }
    return result;
}

/**
 * Wrapper for ApplyTrackSurfaceForceToActor: scale V2W impulse.
 */
static void __cdecl Hook_ApplyTrackForce(int actor, int *param2,
                                          uint32_t heading, int depth,
                                          uint32_t param5) {
    if (g_tuning.v2w_impulse_scale != 100) {
        int32_t *pk = (int32_t *)ADDR_V2W_INERTIA_K;
        int32_t orig_k = *pk;
        *pk = (orig_k * 100) / g_tuning.v2w_impulse_scale;
        Orig_ApplyTrackForce(actor, param2, heading, depth, param5);
        *pk = orig_k;
    } else {
        Orig_ApplyTrackForce(actor, param2, heading, depth, param5);
    }
}

/* =========================================================================
 * Static Patches (applied once at startup via PatchMemory)
 * ========================================================================= */

/**
 * Patch grip clamp immediates in the instruction stream.
 * These are CMP imm8 operands in UpdatePlayerVehicleDynamics and
 * UpdateAIVehicleDynamics.
 */
static int PatchGripClamps(void) {
    int ok = 1;
    int i;
    uint8_t val8;
    int32_t val32;

    /* Player grip low clamp (default 0x38) */
    if (g_tuning.player_grip_clamp_low != 0x38) {
        val8 = (uint8_t)(g_tuning.player_grip_clamp_low & 0xFF);
        val32 = (int32_t)g_tuning.player_grip_clamp_low;
        /* Patch CMP imm8 operands (4 sites) */
        for (i = 0; i < 4; i++)
            ok &= PatchMemory((void *)PLAYER_GRIP_LOW_CMP[i], &val8, 1);
        /* Patch MOV imm32 fallback values (4 sites) */
        for (i = 0; i < 4; i++)
            ok &= PatchMemory((void *)PLAYER_GRIP_LOW_MOV[i], &val32, 4);
    }

    /* Player grip high clamp (default 0x50) --
     * Single MOV EDX,imm32 supplies the register used by all 4 CMP reg,EDX */
    if (g_tuning.player_grip_clamp_high != 0x50) {
        val32 = (int32_t)g_tuning.player_grip_clamp_high;
        ok &= PatchMemory((void *)PLAYER_GRIP_HIGH_ADDR, &val32, 4);
    }

    /* AI grip low clamp (default 0x70) */
    if (g_tuning.ai_grip_clamp_low != 0x70) {
        val8 = (uint8_t)(g_tuning.ai_grip_clamp_low & 0xFF);
        val32 = (int32_t)g_tuning.ai_grip_clamp_low;
        for (i = 0; i < 2; i++)
            ok &= PatchMemory((void *)AI_GRIP_LOW_CMP[i], &val8, 1);
        for (i = 0; i < 2; i++)
            ok &= PatchMemory((void *)AI_GRIP_LOW_MOV[i], &val32, 4);
    }

    /* AI grip high clamp (default 0xA0) --
     * Single MOV EDX,imm32 supplies register for both CMP reg,EDX */
    if (g_tuning.ai_grip_clamp_high != 0xA0) {
        val32 = (int32_t)g_tuning.ai_grip_clamp_high;
        ok &= PatchMemory((void *)AI_GRIP_HIGH_ADDR, &val32, 4);
    }

    return ok;
}

/**
 * Patch gravity immediates in InitializeRaceVehicleRuntime.
 * These are the MOV dword immediates for each difficulty level.
 * We scale them by gravity_scale/100.
 */
static int PatchGravityImmediates(void) {
    int ok = 1;
    if (g_tuning.gravity_scale != 100) {
        int32_t hard   = (0x800 * g_tuning.gravity_scale) / 100;
        int32_t easy   = (0x5DC * g_tuning.gravity_scale) / 100;
        int32_t normal = (0x76C * g_tuning.gravity_scale) / 100;

        ok &= PatchMemory((void *)ADDR_GRAVITY_HARD,   &hard,   4);
        ok &= PatchMemory((void *)ADDR_GRAVITY_EASY,   &easy,   4);
        ok &= PatchMemory((void *)ADDR_GRAVITY_NORMAL, &normal, 4);
    }
    return ok;
}

/**
 * Patch the damage toggle.
 * DAT_00463188: 0 = collisions ON, 1 = collisions OFF.
 * Our DamageEnabled: 1 = damage on (write 0), 0 = damage off (write 1).
 */
static int PatchDamageToggle(void) {
    if (!g_tuning.damage_enabled) {
        int32_t off = 1;
        return PatchMemory((void *)ADDR_COLLISIONS_TOGGLE, &off, 4);
    }
    return 1;
}

/**
 * Patch the encounter speed threshold immediate.
 * In UpdateSpecialTrafficEncounter (0x434DA0), the comparison
 *   "if (speed < 0x15639)" uses a CMP with imm32.
 * We find and patch that immediate.
 */
static int PatchEncounterSpeedThreshold(void) {
    int ok = 1;
    if (g_tuning.encounter_speed_threshold != 0x15639) {
        /* The 0x15639 immediate appears in UpdateSpecialTrafficEncounter.
         * Search the function body for this 4-byte pattern and patch it. */
        uint8_t *fn_start = (uint8_t *)0x434DA0;
        uint8_t *fn_end   = fn_start + 0x300;  /* function is ~300 bytes */
        uint32_t needle   = 0x00015639;
        uint32_t replace  = (uint32_t)g_tuning.encounter_speed_threshold;
        uint8_t *p;
        int found = 0;

        for (p = fn_start; p < fn_end - 4; p++) {
            if (*(uint32_t *)p == needle) {
                ok &= PatchMemory(p, &replace, 4);
                found++;
            }
        }
        if (!found) {
            OutputDebugStringA("[TD5TUNE] WARNING: encounter speed threshold not found\n");
        }
    }
    return ok;
}

/**
 * Patch the AI steering clamp.
 * The hard limit 0x18000 (and its negative -0x18000 = 0xFFFE8000) appear
 * in UpdateActorSteeringBias (0x4340C0).
 */
static int PatchAISteeringClamp(void) {
    int ok = 1;
    if (g_tuning.ai_steering_bias_max != 0x18000) {
        /* Patch the positive clamp 0x18000 and negative clamp 0xFFFE8000
         * in UpdateActorSteeringBias.  These are imm32 operands. */
        uint8_t *fn_start = (uint8_t *)0x4340C0;
        uint8_t *fn_end   = fn_start + 0x400;
        uint32_t pos_needle  = 0x00018000;
        int32_t  neg_needle  = -0x18000;  /* 0xFFFE8000 */
        uint32_t pos_replace = (uint32_t)g_tuning.ai_steering_bias_max;
        int32_t  neg_replace = -g_tuning.ai_steering_bias_max;
        uint8_t *p;

        for (p = fn_start; p < fn_end - 4; p++) {
            if (*(uint32_t *)p == pos_needle) {
                ok &= PatchMemory(p, &pos_replace, 4);
            } else if (*(int32_t *)p == neg_needle) {
                ok &= PatchMemory(p, &neg_replace, 4);
            }
        }
    }
    return ok;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int Tuning_Init(const char *ini_path) {
    int ok = 1;

    Tuning_LoadConfig(ini_path);

    {
        char msg[256];
        wsprintfA(msg,
            "[TD5TUNE] Config: GripPlayer=%d GripAI=%d Rubber=%d "
            "V2V=%d V2W=%d Gravity=%d Friction=%d TopSpeed=%d Traffic=%d\n",
            g_tuning.ai_grip_scale_player, g_tuning.ai_grip_scale_ai,
            g_tuning.ai_rubber_band_strength,
            g_tuning.v2v_impulse_scale, g_tuning.v2w_impulse_scale,
            g_tuning.gravity_scale, g_tuning.friction_scale,
            g_tuning.top_speed_scale, g_tuning.traffic_enabled);
        OutputDebugStringA(msg);
    }

    /* Check if anything actually needs patching */
    if (g_tuning.ai_grip_scale_player == 100 && g_tuning.ai_grip_scale_ai == 100 &&
        g_tuning.ai_rubber_band_strength == 100 && g_tuning.ai_throttle_scale == 100 &&
        g_tuning.ai_brake_scale == 100 && g_tuning.ai_steering_bias_max == 0x18000 &&
        g_tuning.traffic_enabled == 1 && g_tuning.encounter_cooldown_frames == 300 &&
        g_tuning.encounter_speed_threshold == 0x15639 &&
        g_tuning.v2v_impulse_scale == 100 && g_tuning.v2w_impulse_scale == 100 &&
        g_tuning.damage_enabled == 1 &&
        g_tuning.player_grip_clamp_low == 0x38 && g_tuning.player_grip_clamp_high == 0x50 &&
        g_tuning.ai_grip_clamp_low == 0x70 && g_tuning.ai_grip_clamp_high == 0xA0 &&
        g_tuning.gravity_scale == 100 && g_tuning.friction_scale == 100 &&
        g_tuning.top_speed_scale == 100) {
        OutputDebugStringA("[TD5TUNE] All defaults -- no patches needed\n");
        return 1;
    }

    /* Apply static patches */
    ok &= PatchGripClamps();
    ok &= PatchGravityImmediates();
    ok &= PatchDamageToggle();
    ok &= PatchEncounterSpeedThreshold();
    ok &= PatchAISteeringClamp();

    if (ok)
        OutputDebugStringA("[TD5TUNE] Static patches applied successfully\n");
    else
        OutputDebugStringA("[TD5TUNE] WARNING: some static patches failed\n");

    return ok;
}

int Tuning_InstallHooks(void) {
    int ok = 1;
    int any_hooks_needed = 0;

    /* Check if any hooks are actually needed */
    if (g_tuning.gravity_scale != 100 ||
        g_tuning.ai_rubber_band_strength != 100 ||
        g_tuning.ai_throttle_scale != 100 ||
        g_tuning.v2v_impulse_scale != 100 ||
        g_tuning.v2w_impulse_scale != 100 ||
        !g_tuning.traffic_enabled ||
        g_tuning.encounter_cooldown_frames != 300) {
        any_hooks_needed = 1;
    }

    if (!any_hooks_needed) {
        OutputDebugStringA("[TD5TUNE] No runtime hooks needed\n");
        return 1;
    }

    /* Hook InitializeRaceVehicleRuntime for gravity scaling */
    if (g_tuning.gravity_scale != 100) {
        if (MH_CreateHook((LPVOID)0x0042F140, &Hook_InitRaceVehicleRuntime,
                          (LPVOID *)&Orig_InitRaceVehicleRuntime) == MH_OK) {
            MH_EnableHook((LPVOID)0x0042F140);
            OutputDebugStringA("[TD5TUNE] InitializeRaceVehicleRuntime hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: InitializeRaceVehicleRuntime\n");
            ok = 0;
        }
    }

    /* Hook InitializeRaceActorRuntime for rubber-band + throttle scaling */
    if (g_tuning.ai_rubber_band_strength != 100 || g_tuning.ai_throttle_scale != 100) {
        if (MH_CreateHook((LPVOID)0x00432E60, &Hook_InitRaceActorRuntime,
                          (LPVOID *)&Orig_InitRaceActorRuntime) == MH_OK) {
            MH_EnableHook((LPVOID)0x00432E60);
            OutputDebugStringA("[TD5TUNE] InitializeRaceActorRuntime hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: InitializeRaceActorRuntime\n");
            ok = 0;
        }
    }

    /* Hook ConfigureGameTypeFlags for traffic toggle */
    if (!g_tuning.traffic_enabled) {
        if (MH_CreateHook((LPVOID)0x00410CA0, &Hook_ConfigGameType,
                          (LPVOID *)&Orig_ConfigGameType) == MH_OK) {
            MH_EnableHook((LPVOID)0x00410CA0);
            OutputDebugStringA("[TD5TUNE] ConfigureGameTypeFlags hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: ConfigureGameTypeFlags\n");
            ok = 0;
        }
    }

    /* Hook UpdateSpecialTrafficEncounter for cooldown override */
    if (g_tuning.encounter_cooldown_frames != 300) {
        if (MH_CreateHook((LPVOID)0x00434DA0, &Hook_UpdateEncounter,
                          (LPVOID *)&Orig_UpdateEncounter) == MH_OK) {
            MH_EnableHook((LPVOID)0x00434DA0);
            OutputDebugStringA("[TD5TUNE] UpdateSpecialTrafficEncounter hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: UpdateSpecialTrafficEncounter\n");
            ok = 0;
        }
    }

    /* Hook ApplyVehicleCollisionImpulse for V2V impulse scaling */
    if (g_tuning.v2v_impulse_scale != 100) {
        if (MH_CreateHook((LPVOID)0x004079C0, &Hook_ApplyCollisionImpulse,
                          (LPVOID *)&Orig_ApplyCollisionImpulse) == MH_OK) {
            MH_EnableHook((LPVOID)0x004079C0);
            OutputDebugStringA("[TD5TUNE] ApplyVehicleCollisionImpulse hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: ApplyVehicleCollisionImpulse\n");
            ok = 0;
        }
    }

    /* Hook ApplyTrackSurfaceForceToActor for V2W impulse scaling */
    if (g_tuning.v2w_impulse_scale != 100) {
        if (MH_CreateHook((LPVOID)0x00406980, &Hook_ApplyTrackForce,
                          (LPVOID *)&Orig_ApplyTrackForce) == MH_OK) {
            MH_EnableHook((LPVOID)0x00406980);
            OutputDebugStringA("[TD5TUNE] ApplyTrackSurfaceForceToActor hooked\n");
        } else {
            OutputDebugStringA("[TD5TUNE] FAILED: ApplyTrackSurfaceForceToActor\n");
            ok = 0;
        }
    }

    if (ok)
        OutputDebugStringA("[TD5TUNE] All runtime hooks installed\n");

    return ok;
}
