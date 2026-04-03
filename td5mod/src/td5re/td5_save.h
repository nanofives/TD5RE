/**
 * td5_save.h -- Config/cup save/load with XOR encryption and CRC-32
 *
 * Config.td5:  5351 bytes, key="Outta Mah Face !! " (18 chars, VA 0x463F9C)
 * CupData.td5: 12966 bytes, key="Steve Snake says : No Cheating! " (32 chars, VA 0x464084)
 * Both use CRC-32 (standard polynomial 0xEDB88320) for integrity checking.
 *
 * Original functions:
 *   0x40F8D0  WritePackedConfigTd5
 *   0x40FB60  LoadPackedConfigTd5
 *   0x4114F0  WriteCupData
 *   0x411120  SerializeRaceStatusSnapshot
 *   0x411590  LoadContinueCupData
 *   0x4112C0  RestoreRaceStatusSnapshot
 *   0x411630  ValidateCupDataChecksum
 */

#ifndef TD5_SAVE_H
#define TD5_SAVE_H

#include "td5_types.h"

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

/** Initialize internal save-system state. Call once at startup. Returns 1. */
int  td5_save_init(void);

/** Shutdown. Currently a no-op; reserved for future cleanup. */
void td5_save_shutdown(void);

/* ========================================================================
 * Config.td5 -- persistent settings, controls, unlocks, high scores
 *
 * Write: serializes all globals -> CRC-32 -> XOR encrypt -> file
 * Load:  file -> XOR decrypt -> CRC-32 validate -> restore globals
 *
 * If path is NULL, uses "Config.td5" (original default).
 * Returns 1 on success, 0 on failure (missing file / bad CRC).
 * ======================================================================== */

int  td5_save_load_config(const char *path);
int  td5_save_write_config(const char *path);

/* ========================================================================
 * CupData.td5 -- mid-cup state snapshot for "Continue Cup"
 *
 * write: SerializeRaceStatusSnapshot + WriteCupData
 * load:  LoadContinueCupData (read + decrypt + RestoreRaceStatusSnapshot)
 * validate: ValidateCupDataChecksum (check without restoring)
 *
 * If path is NULL, uses "CupData.td5" (original default).
 * Returns 1 on success, 0 on failure.
 * ======================================================================== */

int  td5_save_load_cup_data(const char *path);
int  td5_save_write_cup_data(const char *path);

/** Validate CupData.td5 checksum without restoring state.
 *  Used to enable/disable the "Continue Cup" button.
 *  @param path         File path (NULL for default "CupData.td5").
 *  @param expected_crc The CRC-32 value to compare against.
 *  @return 1 if file exists and CRC matches, 0 otherwise. */
int  td5_save_validate_cup_checksum(const char *path, uint32_t expected_crc);

/* ========================================================================
 * Encryption primitives
 *
 * XOR cipher: encrypted[i] = plaintext[i] ^ key[i % key_len] ^ 0x80
 * Self-inverse (encrypt == decrypt).
 * ======================================================================== */

void td5_save_xor_encrypt(uint8_t *data, size_t size, const char *key);
void td5_save_xor_decrypt(uint8_t *data, size_t size, const char *key);

/* ========================================================================
 * CRC-32 (delegates to td5_crc32 in td5_types.h)
 * ======================================================================== */

uint32_t td5_save_crc32(const uint8_t *data, size_t size);

/* ========================================================================
 * Game options access
 * ======================================================================== */

/** Returns pointer to the mutable game options struct.
 *  Fields: circuit_laps, checkpoint_timers, traffic, cops,
 *          difficulty, dynamics, collisions_3d. */
TD5_GameOptions *td5_save_get_game_options(void);

/* ========================================================================
 * High-score (NPC) table access
 *
 * 26 groups (one per track), 164 bytes each, 5 entries per group.
 * Cast the returned pointer: ((const TD5_NpcGroup *)ptr)[group_index]
 * ======================================================================== */

/** Returns read-only pointer to the 4264-byte NPC group table (0x4643B8).
 *  Returns NULL only if the save module is not initialized. */
const uint8_t *td5_save_get_npc_table(void);

/** Returns pointer to a specific NPC group (0-25). NULL if out of range. */
const TD5_NpcGroup *td5_save_get_npc_group(int group_index);

/** Returns the speed-units setting (0=MPH, 1=KPH). */
int td5_save_get_speed_units(void);

/** Returns the configured circuit lap count (2/4/6/8). */
int td5_save_get_circuit_lap_count(void);

/* ========================================================================
 * Unlock system
 *
 * Car lock table:   37 entries, 0=unlocked, 1=locked.
 * Track lock table: 26 entries, 0=locked, 1=unlocked.
 * ======================================================================== */

int  td5_save_is_car_unlocked(int car_index);
int  td5_save_is_track_unlocked(int track_index);
void td5_save_unlock_car(int car_index);
void td5_save_unlock_track(int track_index);

/* ========================================================================
 * Volume accessors
 * ======================================================================== */

int  td5_save_get_sfx_volume(void);
void td5_save_set_sfx_volume(int v);
int  td5_save_get_music_volume(void);
void td5_save_set_music_volume(int v);

#endif /* TD5_SAVE_H */
