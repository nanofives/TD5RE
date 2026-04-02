/**
 * td5_tuning.h - AI, Collision, and Physics Tuning Module
 *
 * Reads [AITuning], [CollisionTuning], and [PhysicsTuning] sections from
 * scripts\td5_mod.ini and patches the hardcoded constants in TD5_d3d.exe
 * at startup.
 *
 * All addresses verified against Ghidra decompilation (port 8195).
 */

#ifndef TD5_TUNING_H
#define TD5_TUNING_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load tuning parameters from INI and apply all patches.
 * Call after MinHook is initialized and before the game starts.
 *
 * @param ini_path  Full path to td5_mod.ini
 * @return 1 on success, 0 if any patch failed
 */
int Tuning_Init(const char *ini_path);

/**
 * Hooks that must be installed via MinHook (called from InstallHooks).
 * Tuning_Init loads config; this installs the runtime hooks for parameters
 * that cannot be patched as static constants.
 *
 * @return 1 on success, 0 on failure
 */
int Tuning_InstallHooks(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_TUNING_H */
