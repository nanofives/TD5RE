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

/** Delete the persisted cup-resume state (td5re_cup.ini + any legacy
 *  CupData.td5). Called after a cup is won/abandoned. */
void td5_save_delete_cup_data(void);

/** TD5RE divergent overlay self-test. Writes "test_cup_roundtrip.td5"
 *  in cwd, reloads, and asserts that g_td5.car_index +
 *  g_td5.ai_car_indices[1..5] survive the round-trip. Mutates and then
 *  restores g_td5 + s_actor_table; safe to run before any race init.
 *  Returns 1 on PASS, 0 on FAIL. */
int  td5_save_test_cup_roundtrip(void);

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

/** Returns mutable pointer to a specific NPC group for in-place name-entry write.
 *  [CONFIRMED @ 0x00413BC0 case 4]: original writes directly into g_npcRacerGroupTable. */
TD5_NpcGroup *td5_save_get_npc_group_mutable(int group_index);

/* ========================================================================
 * High-score TD5RE extensions (parallel to the RE-faithful TD5_NpcEntry)
 *
 * TD5_NpcEntry / TD5_NpcGroup mirror the original 32/164-byte binary layout
 * exactly (they round-trip through the legacy Config.td5 buffer), so they cannot
 * grow. These parallel per-entry extensions hold TD5RE-only data that doesn't
 * fit: the FULL display name (>15 chars, e.g. "Michael Schumacher") and the
 * race-results-parity metrics (collisions, air time) now shown in the High
 * Scores table. Indexed [group][entry], kept in lockstep with the sorted NPC
 * entries by the insert helpers below. Persisted in td5re_progress.ini. */
typedef struct TD5_NpcEntryExt {
    char    full_name[32];   /* full display name; "" => fall back to entry.name[16] */
    int32_t collisions;      /* race collisions   (== race-results COLLISIONS column) */
    int32_t air_ticks;       /* airborne ticks @30fps (== race-results AIR TIME column) */
} TD5_NpcEntryExt;

/** Read-only extension for a TD5 NPC group entry (group 0-25, entry 0-4). NULL if
 *  out of range. full_name[0]=='\0' means "no full name — display entry.name". */
const TD5_NpcEntryExt *td5_save_get_npc_ext(int group_index, int entry);

/** Read-only extension for a TD6 record entry (level, entry 0-4). NULL if out of
 *  range or the level has no records. */
const TD5_NpcEntryExt *td5_save_get_td6_ext(int td6_level, int entry);

/** Insert one result into TD5 NPC group `group_index`, keeping the 5 entries
 *  sorted by the group's header score type (0/1/4 = time, lower better; 2 =
 *  points, higher better). Shifts both the entry and its extension in lockstep,
 *  writes name/score/car/avg/top plus the extension (full_name/collisions/
 *  air_ticks), and persists to td5re_progress.ini. Returns inserted rank [0..4]
 *  or -1 if it didn't place / args invalid. Used by both the single-player name-
 *  entry flow and multiplayer split-screen auto-registration. */
int td5_save_npc_record_insert(int group_index, const char *full_name,
                               int32_t score, int car_id,
                               int32_t avg_speed, int32_t top_speed,
                               int32_t collisions, int32_t air_ticks);

/* ========================================================================
 * TD6 high-score records (port enhancement #2b 2026-06-16)
 *
 * The original 26-group NPC high-score table only covers the 26 authored TD5
 * tracks (td5_save_get_npc_group returns NULL for >= 26). TD6 tracks have NO
 * authored group, so the post-race high-score screen used to clamp onto a TD5
 * group and show its FAKE seed names. These accessors keep a separate, small
 * per-TD6-track record table (same TD5_NpcGroup layout) holding ONLY genuine
 * runs the player has set this/previous sessions — never placeholder names.
 * Persisted in td5re_progress.ini under [TD6Records.LevelNN]; loaded lazily;
 * gated by TD5RE_TD6_NO_PLACEHOLDER_SCORES (default on — see the .c). When the
 * knob is off these return NULL / -1 so the legacy clamp path is unaffected.
 * `td6_level` is the TD6 level number (td5_asset_td6_level_for_slot), 1-based.
 * ======================================================================== */

/** Read-only TD6 record group for `td6_level`. NULL if the level has no stored
 *  records yet, the level is out of range, or the feature is disabled. */
const TD5_NpcGroup *td5_save_get_td6_record_group(int td6_level);

/** Insert a genuine record into the TD6 table for `td6_level`, keeping the 5
 *  entries sorted by `score_type` (0/1/4 = time, lower better; 2 = points,
 *  higher better). Persists to disk. Returns the inserted rank [0..4], or -1
 *  if it didn't qualify / the feature is off / args are invalid. */
int td5_save_td6_record_insert(int td6_level, int score_type,
                               const char *name, int32_t score,
                               int car_id, int32_t avg_speed, int32_t top_speed,
                               int32_t collisions, int32_t air_ticks);

/** Returns the speed-units setting (0=MPH, 1=KPH). */
int td5_save_get_speed_units(void);

/** Set the speed-units setting (0=MPH, 1=KPH). */
void td5_save_set_speed_units(int units);

/** Set the camera damping setting. */
void td5_save_set_camera_damping(int d);

/** Set the sound mode (0=stereo, 1=mono). */
void td5_save_set_sound_mode(int mode);

/** Returns the configured circuit lap count (2/4/6/8). */
int td5_save_get_circuit_lap_count(void);

/** Returns the saved display-mode ordinal (config.td5 byte 0xBD).
 *  Mirrors gSelectedDisplayModeOrdinal @ 0x00497334 — the index into the
 *  enumerated display-mode list selected by the user. */
int  td5_save_get_display_mode(void);
void td5_save_set_display_mode(int ordinal);

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

/** Returns the maximum car index (exclusive) visible to the player.
 *  Cars [0..max-1] appear in the selector; higher indices are hidden. */
int  td5_save_get_max_unlocked_car(void);

/** Returns 1 if all_cars_unlocked flag is set (all 37 available). */
int  td5_save_get_all_cars_unlocked(void);

/** Returns the cup tier bitmask (bits 0-2: Championship/Challenge/Pitbull). */
int  td5_save_get_cup_tier(void);

/** Copy lock tables into caller-supplied arrays.
 *  car_locks:   37-byte array, filled with 0=unlocked, nonzero=locked.
 *  track_locks: 26-byte array, filled with 0=unlocked, nonzero=locked.
 *  (Note: internal track lock sense is inverted: 1=unlocked, 0=locked.
 *   These functions normalize to 0=unlocked for the caller.) */
void td5_save_get_car_lock_table(uint8_t *out_car_locks, int count);
void td5_save_get_track_lock_table(uint8_t *out_track_locks, int count);

/** Process cup tier state and apply unlocks for completed cups.
 *  Call after loading Config.td5 or after a cup is won.
 *  game_type: the cup that was just won (1-6), or -1 to just refresh.
 *  Returns the number of newly unlocked items. */
int  td5_save_apply_cup_unlocks(int game_type);
/** Like td5_save_apply_cup_unlocks but also fills separate car/track counts.
 *  [CONFIRMED @ 0x423A80]: original tracks g_cupSchedule_currentCup (cars) / g_cupSchedule_currentRound (tracks) separately. */
int  td5_save_apply_cup_unlocks_ex(int game_type, int *cars_out, int *tracks_out);

/* ========================================================================
 * Cup state sync -- bridge save module statics <-> game globals
 *
 * In the original binary, save and frontend shared global addresses.
 * In the source port, the save module has private copies that must be
 * synced before write and after load.
 * ======================================================================== */

/** Copy current game state (g_td5 + actor table) into save module cup statics.
 *  Call immediately before td5_save_write_cup_data(). */
void td5_save_sync_cup_from_game(int race_within_series);

/** Copy save module cup statics back to game state (g_td5 + actor table).
 *  Call immediately after td5_save_load_cup_data().
 *  Returns the restored game_type, or -1 if no valid cup state. */
int  td5_save_sync_cup_to_game(int *out_race_within_series);

/** Validate CupData.td5 integrity without restoring state.
 *  Reads file, decrypts, checks CRC.  Returns 1 if valid, 0 otherwise. */
int  td5_save_is_cup_valid(const char *path);

/* ========================================================================
 * Volume accessors
 * ======================================================================== */

int  td5_save_get_sfx_volume(void);
void td5_save_set_sfx_volume(int v);
int  td5_save_get_music_volume(void);
void td5_save_set_music_volume(int v);
float td5_save_get_view_distance(void);
void  td5_save_set_view_distance(float v);

/* CATCHUP / rubber-band assist level (0..9; 0 = off). Persisted across launches.
 * Set by the S05 Multiplayer Options toggle; read by the AI rubber-band and the
 * 2-player steering-bias swing. [S06 2026-06-04 catchup restore] */
int  td5_save_get_catchup_assist(void);
void td5_save_set_catchup_assist(int v);

/* Controller / keyboard binding buffers — mutable pointers for binding-screen capture.
 * td5_save_get_controller_bindings_mutable: TD5_MAX_HUMAN_PLAYERS*9 dwords, flat
 *   [player*9+slot] ([PORT ENHANCEMENT 2026-06] grown from 18 to 81 for 9 players;
 *   first 18 still round-trip through the legacy Config.td5 blob).
 * td5_save_get_p1/p2_custom_bindings_mutable: 98 dwords (392 bytes), first 16 bytes
 *   used as uint8_t[16] keyboard scancode capture buffer for the respective player.
 * After writing, call td5_save_write_config() to persist.
 * [CONFIRMED @ 0x463FC4 / 0x4978C0 / 0x497330] */
uint32_t *td5_save_get_controller_bindings_mutable(void);
uint32_t *td5_save_get_p1_custom_bindings_mutable(void);
uint32_t *td5_save_get_p2_custom_bindings_mutable(void);
/* [PORT ENHANCEMENT 2026-06] per-action joystick bindings, flat
 * [player*TD5_JSBIND_ACTIONS + action] (10 codes/player). */
uint32_t *td5_save_get_action_bindings_mutable(void);

/* Persisted per-player input device index (Config.td5 +0x20/+0x21 for p1/p2):
 * 0 = keyboard, >=1 = 1-based joystick index. */
uint32_t td5_save_get_p1_device_index(void);
uint32_t td5_save_get_p2_device_index(void);
/* [PORT ENHANCEMENT 2026-06] generic per-player accessors (player 0..8). */
uint32_t td5_save_get_player_device_index(int player);
void     td5_save_set_player_device_index(int player, uint32_t idx);

/* ========================================================================
 * Player profiles -- persistent name + colour + car presets
 *
 * [PORT ENHANCEMENT 2026-06 #11] Persistent store for the multiplayer
 * name/colour frontend (td5_fe_race.c). A player configures a display name,
 * an MP accent/identity colour and their car preset; the profile is saved so
 * it can be reloaded in a future run. Persisted in td5re_progress.ini under
 * the [Profiles] section (this module already owns that file). The store is
 * loaded lazily on first access and rewritten to disk on every save/delete.
 * ======================================================================== */

typedef struct TD5_Profile {
    char name[16];     /* player display name */
    int  accent;       /* MP accent/identity colour index */
    int  car;          /* last selected car index */
    int  paint;        /* car paint/variant index */
    int  color;        /* car colour index (if distinct from paint) */
    int  trans;        /* transmission/auto pref (0/1) */
} TD5_Profile;
#define TD5_MAX_PROFILES 16

/** Number of stored profiles (0..TD5_MAX_PROFILES). */
int td5_save_profile_count(void);

/** Fetch profile idx into *out. Returns 1 on success, 0 if idx out of range
 *  or out is NULL. */
int td5_save_profile_get(int idx, TD5_Profile *out);

/** UPSERT a profile by name (case-insensitive): if a stored profile shares
 *  the (trimmed) name it is overwritten in place, otherwise a new slot is
 *  appended. Persists to disk. Returns the slot index, or -1 if the store is
 *  full, p is NULL, or the name is empty. */
int td5_save_profile_save(const TD5_Profile *p);

/** Delete profile idx (slots after it shift down). Persists to disk.
 *  Returns 1 on success, 0 if idx out of range. */
int td5_save_profile_delete(int idx);

/* ---- First-race tutorial overlay "seen" flag (PORT ENHANCEMENT 2026-06) ----
 * Persisted in td5re_progress.ini under [Tutorial] Seen. 1 once the player has
 * dismissed the controller-tutorial overlay, so it never shows again. */
int  td5_save_get_tutorial_seen(void);
void td5_save_set_tutorial_seen(int seen);

#endif /* TD5_SAVE_H */
