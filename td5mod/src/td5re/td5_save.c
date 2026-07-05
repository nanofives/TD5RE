/**
 * td5_save.c -- Config/cup save/load with XOR encryption
 *
 * Reverse-engineered from TD5_d3d.exe original functions:
 *   0x40F8D0  WritePackedConfigTd5       -- DONE
 *   0x40FB60  LoadPackedConfigTd5        -- DONE
 *   0x411120  SerializeRaceStatusSnapshot-- DONE
 *   0x4112C0  RestoreRaceStatusSnapshot  -- DONE
 *   0x4114F0  WriteCupData               -- DONE
 *   0x411590  LoadContinueCupData        -- DONE
 *   0x411630  ValidateCupDataChecksum    -- DONE
 */

#include "td5_save.h"
#include "td5_platform.h"
#include "td5re.h"
#include "td5_game.h"
#include "td5_config.h"  /* shared TD5RE_* env-knob helpers */
#include <string.h>
#include <stdio.h>  /* remove() in td5_save_test_cup_roundtrip */
#include <stdarg.h> /* vsnprintf in the INI text builder */
#include <stdlib.h> /* strtol when parsing comma-separated INI lists */
#include <windows.h>
#include <winhttp.h>

#define LOG_TAG "save"

/* ========================================================================
 * Constants
 * ======================================================================== */

/** CRC-32 placeholder written to bytes [0..3] (Config) or [0x0C..0x0F]
 *  (CupData) before computing the checksum. Little-endian 0x00000010. */
#define TD5_CRC_PLACEHOLDER         0x00000010u

/** Config.td5 read buffer size (original uses 0x1800 = 6144). */
#define TD5_CONFIG_READ_BUF_SIZE    0x1800

/** CupData.td5 read buffer size (original uses 0x4000 = 16384). */
#define TD5_CUPDATA_READ_BUF_SIZE   0x4000

/** Config.td5 file name used by the original binary. */
#define TD5_CONFIG_FILENAME         "Config.td5"

/** CupData.td5 file name used by the original binary. */
#define TD5_CUPDATA_FILENAME        "CupData.td5"

/* ------------------------------------------------------------------------
 * Organized human-readable INI files that REPLACE the binary Config.td5 /
 * CupData.td5 (user request 2026-06-03). Settings live in td5re.ini; the
 * three files below hold the data that does not belong in the launch-config
 * INI:
 *   td5re_input.ini     -- controller + keyboard bindings, device/FF config
 *   td5re_progress.ini  -- high-score table, unlock/cheat/progression state
 *   td5re_cup.ini       -- "Continue Cup" resume state (no actor snapshot)
 * On first launch any existing legacy Config.td5 / CupData.td5 is imported
 * once, then renamed to *.migrated so it is never read again.
 * ------------------------------------------------------------------------ */
#define TD5RE_INPUT_INI             "td5re_input.ini"
#define TD5RE_PROGRESS_INI          "td5re_progress.ini"
#define TD5RE_CUP_INI               "td5re_cup.ini"
#define TD5_CONFIG_MIGRATED         "Config.td5.migrated"
#define TD5_CUPDATA_MIGRATED        "CupData.td5.migrated"

/** Max resolved-path length for the exe-relative INI files. */
#define TD5_CFGINI_PATH_MAX         600

/* Forward declarations for the INI persistence layer (defined after the
 * module statics, since they read/write them directly). */
static const char *cfgini_input_path(void);
static const char *cfgini_progress_path(void);
static const char *cfgini_cup_path(void);
static int  cfgini_write_input(void);
static int  cfgini_read_input(void);
static int  cfgini_write_progress(void);
static int  cfgini_read_progress(void);
static int  cfgini_write_cup(const char *path);
static int  cfgini_read_cup(const char *path);
static int  cup_load_binary_file(const char *path);   /* legacy CupData.td5 reader */
static int  cup_is_binary_valid(const char *path);    /* legacy CRC self-check */
static void profiles_ensure_loaded(void);             /* lazy [Profiles] read */
static void profiles_read(void);                      /* read [Profiles] from progress.ini */
static void td6_records_ensure_loaded(void);          /* lazy [TD6Records] read */
static void td6_records_read(void);                   /* read [TD6Records] from progress.ini */

/* ========================================================================
 * Config.td5 buffer layout (5351 bytes)
 *
 * The original serializes into a flat byte buffer at 0x48F384.
 * We mirror that layout exactly with a packed struct.
 * ======================================================================== */

#define TD5_CONFIG_NPC_GROUPS       26
#define TD5_CONFIG_NPC_GROUP_SIZE   164  /* 0xA4 bytes per group */
#define TD5_CONFIG_NPC_TABLE_SIZE   (TD5_CONFIG_NPC_GROUPS * TD5_CONFIG_NPC_GROUP_SIZE) /* 4264 */
#define TD5_CONFIG_NUM_CARS         37
#define TD5_CONFIG_NUM_TRACKS       26
#define TD5_CONFIG_NUM_CHEATS       26
#define TD5_CONFIG_BINDING_DWORDS   18
#define TD5_CONFIG_DEV_DESC_DWORDS  8
#define TD5_CONFIG_CUSTOM_BIND_DWORDS 0x62  /* 98 dwords = 392 bytes */

#pragma pack(push, 1)
typedef struct TD5_ConfigBuffer {
    /* 0x0000 */ uint32_t crc32;
    /* 0x0004 */ int32_t  game_options[7];                                  /* 28 bytes */
    /* 0x0020 */ uint8_t  p1_device_index;
    /* 0x0021 */ uint8_t  p2_device_index;
    /* 0x0022 */ int32_t  ff_config_a;
    /* 0x0026 */ int32_t  ff_config_b;
    /* 0x002A */ int32_t  ff_config_c;
    /* 0x002E */ int32_t  ff_config_d;
    /* 0x0032 */ uint32_t controller_bindings[TD5_CONFIG_BINDING_DWORDS];    /* 72 bytes */
    /* 0x007A */ uint32_t p1_device_desc[TD5_CONFIG_DEV_DESC_DWORDS];       /* 32 bytes */
    /* 0x009A */ uint32_t p2_device_desc[TD5_CONFIG_DEV_DESC_DWORDS];       /* 32 bytes */
    /* 0x00BA */ uint8_t  sound_mode;
    /* 0x00BB */ uint8_t  sfx_volume;
    /* 0x00BC */ uint8_t  music_volume;
    /* 0x00BD */ int32_t  display_mode_ordinal;
    /* 0x00C1 */ int32_t  fog_enabled;
    /* 0x00C5 */ int32_t  speed_units;
    /* 0x00C9 */ int32_t  camera_damping;
    /* 0x00CD */ uint32_t p1_custom_bindings[TD5_CONFIG_CUSTOM_BIND_DWORDS]; /* 392 bytes */
    /* 0x0255 */ uint32_t p2_custom_bindings[TD5_CONFIG_CUSTOM_BIND_DWORDS]; /* 392 bytes */
    /* 0x03DD */ uint8_t  split_screen_mode;
    /* 0x03DE */ uint8_t  catchup_assist;
    /* 0x03DF */ uint8_t  camera_byte_a;                                     /* 0x482F48 */
    /* 0x03E0 */ uint8_t  camera_byte_b;                                     /* 0x482F49 */
    /* 0x03E1 */ uint8_t  npc_group_table[TD5_CONFIG_NPC_TABLE_SIZE];       /* 4264 bytes -> ends at 0x1489 */
    /* 0x1489 */ uint8_t  reserved_zero;                                     /* always 0 */
    /* 0x148A */ uint8_t  music_track;                                       /* CD audio track index */
    /* 0x148B */ uint8_t  cup_tier_state;                                    /* masked to 3 bits */
    /* 0x148C */ uint8_t  max_unlocked_car;
    /* 0x148D */ uint8_t  all_cars_unlocked;
    /* 0x148E */ uint8_t  track_locks[TD5_CONFIG_NUM_TRACKS];               /* 26 bytes */
    /* 0x14A8 */ uint8_t  car_locks[TD5_CONFIG_NUM_CARS];                   /* 37 bytes */
    /* 0x14CD */ uint8_t  cheat_flags[TD5_CONFIG_NUM_CHEATS];               /* 26 bytes */
    /* Total:  0x14E7 = 5351 bytes */
} TD5_ConfigBuffer;
#pragma pack(pop)

/* Verify buffer size at compile time. */
_Static_assert(sizeof(TD5_ConfigBuffer) == TD5_CONFIG_FILE_SIZE,
               "TD5_ConfigBuffer size mismatch");

/* ========================================================================
 * CupData.td5 buffer layout (12966 bytes)
 * ======================================================================== */

#define TD5_CUP_SCHEDULE_DWORDS    0x1E  /* 30 dwords = 120 bytes */
#define TD5_CUP_RESULTS_DWORDS     0x1E  /* 30 dwords = 120 bytes */
#define TD5_CUP_ACTOR_DWORDS       0xC5C /* 3164 dwords = 12656 bytes */
#define TD5_CUP_SLOT_STATE_DWORDS  6     /* 6 x 4 bytes = 24 bytes */

#pragma pack(push, 1)
typedef struct TD5_CupDataBuffer {
    /* 0x0000 */ uint8_t  game_type;
    /* 0x0001 */ uint8_t  race_index;
    /* 0x0002 */ uint8_t  npc_group_index;
    /* 0x0003 */ uint8_t  track_opponent_state;
    /* 0x0004 */ uint8_t  race_rule_variant;
    /* 0x0005 */ uint8_t  time_trial_flag;
    /* 0x0006 */ uint8_t  wanted_flag;
    /* 0x0007 */ uint8_t  difficulty_tier;
    /* 0x0008 */ uint8_t  checkpoint_mode;
    /* 0x0009 */ uint8_t  traffic_enabled;
    /* 0x000A */ uint8_t  special_encounter;
    /* 0x000B */ uint8_t  circuit_lap_count;
    /* 0x000C */ uint32_t crc32;
    /* 0x0010 */ uint32_t race_schedule[TD5_CUP_SCHEDULE_DWORDS];           /* 120 bytes */
    /* 0x0088 */ uint32_t race_results[TD5_CUP_RESULTS_DWORDS];            /* 120 bytes */
    /* 0x0100 */ uint32_t actor_state[TD5_CUP_ACTOR_DWORDS];               /* 12656 bytes -- 6 slots x 0x388 */
    /* 0x3270 */ uint32_t slot_state[TD5_CUP_SLOT_STATE_DWORDS];           /* 24 bytes */
    /* 0x3288 */ uint32_t masters_schedule_base;
    /* 0x328C */ uint32_t p2_cup_schedule_index;                                   /* VERIFIED: 0x48F310, addr calc 0x493E38-0x490BAC=0x328C */
    /* 0x3290 */ uint32_t p1_cup_schedule_index;                                   /* VERIFIED: 0x48F314, addr calc 0x493E3C-0x490BAC=0x3290 */
    /* 0x3294 */ uint16_t p1_cup_completion_bitmask;                                    /* VERIFIED: 0x48F318, addr calc 0x493E40-0x490BAC=0x3294 (2 bytes, not 4) */
    /* 0x3296 */ uint8_t  p1_selected_cup_id;                                    /* VERIFIED: 0x48F31A, addr calc 0x493E42-0x490BAC=0x3296 */
    /* 0x3297 */ uint32_t masters_encounter_flags;                           /* VERIFIED: 0x48F324, addr calc 0x493E43-0x490BAC=0x3297 */
    /* 0x329B */ uint32_t p1_masters_unlock_bitmask;                                   /* VERIFIED: 0x48F328, addr calc 0x493E47-0x490BAC=0x329B */
    /* 0x329F */ uint32_t p2_masters_unlock_bitmask;                                   /* VERIFIED: 0x48F32C, addr calc 0x493E4B-0x490BAC=0x329F */
    /* 0x32A3 */ uint16_t p2_cup_completion_bitmask;                                    /* VERIFIED: 0x48F330, addr calc 0x493E4F-0x490BAC=0x32A3 */
    /* 0x32A5 */ uint8_t  p2_cup_lock_flag;                                    /* VERIFIED: 0x48F332, addr calc 0x493E51-0x490BAC=0x32A5 */
    /* Remaining bytes to fill 0x32A6... */
} TD5_CupDataBuffer;
#pragma pack(pop)

/*
 * NOTE: The CupData tail region (0x3288..0x32A5) has complex field packing
 * that the decompiler shows as individual byte/word/dword stores at
 * non-aligned offsets. Rather than model every field in a packed struct
 * (which would be fragile), we treat the entire 12966-byte cup snapshot
 * as a flat uint8_t array and use memcpy for serialization/deserialization.
 * This matches the original code's behavior exactly.
 */

/* ========================================================================
 * Module-private global state
 *
 * These mirror the original binary's global variables. In the source port,
 * external modules interact with these through the td5_save API; in the
 * original binary they were scattered across .bss/.data segments.
 * ======================================================================== */

/** Config.td5 serialization buffer (5351 bytes, mirrors 0x48F384). */
static uint8_t s_config_buf[TD5_CONFIG_FILE_SIZE];

/** CupData.td5 snapshot buffer.
 *
 * Sized to TD5_CUPDATA_EXT_FILE_SIZE (12998 B) to hold both the original
 * 12966-byte payload and the 32-byte port-only overlay (magic + 6 car
 * indices). Original-format files (12966 B) populate only the prefix.
 */
static uint8_t s_cup_buf[TD5_CUPDATA_EXT_FILE_SIZE];

/** CupData snapshot byte count (mirrors DAT_00494BBC). Either
 *  TD5_CUPDATA_FILE_SIZE (legacy/original-format) or
 *  TD5_CUPDATA_EXT_FILE_SIZE (port extended with overlay). */
static uint32_t s_cup_buf_size;

/** Per-slot car indices recovered from the overlay (slots 0..5). Set by
 *  cup_deserialize_from_buffer when a valid overlay is present; pushed
 *  into g_td5 by td5_save_sync_cup_to_game. -1 = no overlay (legacy
 *  format), so existing g_td5 values are kept. */
static int s_overlay_car_indices[TD5_CUPDATA_OVERLAY_NUM_SLOTS];
static int s_overlay_present;

/* -- Globals that feed into Config.td5 -- */
static TD5_GameOptions s_game_options;                        /* 0x466000 */
static uint32_t s_p1_device_index;                            /* 0x497A58 */
static uint32_t s_p2_device_index;                            /* 0x465FF4 */
/* [PORT ENHANCEMENT 2026-06] per-player device index for up to 9 split-screen
 * players. [0]/[1] mirror s_p1/p2 (legacy Config.td5 + back-compat); [2..8] are
 * persisted only in td5re_input.ini. 0 = keyboard, >=1 = 1-based joystick. */
static uint32_t s_player_device_index[TD5_MAX_HUMAN_PLAYERS];
static int32_t  s_ff_config[4];                               /* 0x464054, 0x46405C, 0x464058, 0x464060 */
/* [PORT ENHANCEMENT 2026-06] runtime joystick binding table grown from 2 to 9
 * players (9 dwords each). The legacy Config.td5 blob still carries only the
 * first 18 dwords (2 players); players 3-9 persist via td5re_input.ini. */
static uint32_t s_controller_bindings[TD5_MAX_HUMAN_PLAYERS * 9];    /* 0x463FC4 (first 18) */
/* [PORT ENHANCEMENT 2026-06] per-action joystick bindings (10 codes/player):
 * each action mapped to a button or axis/trigger (TD5_JSBIND_*). 0 = unbound →
 * the in-race poll falls back to the default mapping. Persisted in td5re_input.ini
 * [ControllerActions]; not part of the legacy Config.td5 blob. */
static uint32_t s_js_action_bind[TD5_MAX_HUMAN_PLAYERS * TD5_JSBIND_ACTIONS];
static uint32_t s_p1_device_desc[8];                          /* 0x465660 (write) / 0x4656A0 (read) */
static uint32_t s_p2_device_desc[8];                          /* 0x465680 (write) / 0x4656C0 (read) */
static uint32_t s_p1_device_desc_backup[8];                   /* 0x4656A0 (read target) */
static uint32_t s_p2_device_desc_backup[8];                   /* 0x4656C0 (read target) */
static uint32_t s_sound_mode;                                 /* 0x465FE8 */
static uint32_t s_sfx_volume;                                 /* 0x465FEC */
static uint32_t s_music_volume;                               /* 0x465FF0 */
static float    s_view_distance_frac = 1.0f;                  /* 0x466EA8: runtime only, not persisted.
                                                                * Original default = 0.65 [0x0042AA27], source port uses 1.0. */

int td5_save_get_sfx_volume(void)       { return (int)s_sfx_volume; }
void td5_save_set_sfx_volume(int v)     { if (v < 0) v = 0; if (v > 100) v = 100; s_sfx_volume = (uint32_t)v; }
int td5_save_get_music_volume(void)     { return (int)s_music_volume; }
void td5_save_set_music_volume(int v)   { if (v < 0) v = 0; if (v > 100) v = 100; s_music_volume = (uint32_t)v; }
float td5_save_get_view_distance(void)  { return s_view_distance_frac; }
void td5_save_set_view_distance(float v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; s_view_distance_frac = v; }

static int32_t  s_display_mode;                               /* 0x466020 */
static int32_t  s_fog_enabled;                                /* 0x466024 */
static int32_t  s_speed_units;                                /* 0x466028 */
void td5_save_set_speed_units(int u) { s_speed_units = u; }
static int32_t  s_camera_damping;                             /* 0x46602C */
void td5_save_set_camera_damping(int d) { s_camera_damping = d; }
void td5_save_set_sound_mode(int m) { s_sound_mode = m; }
static uint32_t s_p1_custom_bindings[0x62];                   /* 0x4978C0 */
static uint32_t s_p2_custom_bindings[0x62];                   /* 0x497330 */

uint32_t *td5_save_get_controller_bindings_mutable(void) { return s_controller_bindings; }
uint32_t *td5_save_get_action_bindings_mutable(void)     { return s_js_action_bind; }
uint32_t *td5_save_get_p1_custom_bindings_mutable(void)  { return s_p1_custom_bindings; }
uint32_t *td5_save_get_p2_custom_bindings_mutable(void)  { return s_p2_custom_bindings; }

/* Persisted per-player input device index (Config.td5 +0x20/+0x21 for p1/p2):
 * 0 = keyboard, >=1 = 1-based joystick index. */
uint32_t td5_save_get_p1_device_index(void)  { return s_player_device_index[0]; }
uint32_t td5_save_get_p2_device_index(void)  { return s_player_device_index[1]; }

/* [PORT ENHANCEMENT 2026-06] generic per-player device index accessors (0..8). */
uint32_t td5_save_get_player_device_index(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    return s_player_device_index[player];
}
void td5_save_set_player_device_index(int player, uint32_t idx)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    s_player_device_index[player] = idx;
    if (player == 0) s_p1_device_index = idx;   /* keep legacy blob fields in sync */
    if (player == 1) s_p2_device_index = idx;
}

static uint32_t s_split_screen_mode;                          /* 0x497A5C */
/* CATCHUP / rubber-band assist level (orig g_twoPlayerCatchupAssist @ 0x465FF8,
 * range 0..9). 0 = off; >0 = catchup ON. Default 1 so the AI rubber-band is on
 * (softened) on a fresh install — matches the original's always-present catchup
 * and the user-reported "catchup too aggressive" baseline. The S05 Multiplayer
 * Options toggle drives this via td5_save_set_catchup_assist(); td5_ai.c reads it
 * to gate/soften the rubber-band, and td5_input.c maps it to the steering-bias
 * swing. [S06 2026-06-04 catchup restore] */
static uint32_t s_catchup_assist = 1;                         /* 0x465FF8 */
static uint8_t  s_camera_byte_a;                              /* 0x482F48 */
static uint8_t  s_camera_byte_b;                              /* 0x482F49 */
static uint32_t s_music_track;                                /* 0x466840 */
static int      s_tutorial_seen;   /* [PORT 2026-06] first-race controller tutorial dismissed */

/* CATCHUP / rubber-band assist accessors. Persisted via the config buffer
 * (catchup_assist byte). The frontend (S05 Multiplayer Options toggle) sets it;
 * td5_ai.c / td5_input.c read it. Clamped to the original 0..9 range; 0 = off.
 * [S06 2026-06-04 catchup restore] */
int  td5_save_get_catchup_assist(void)   { return (int)s_catchup_assist; }
void td5_save_set_catchup_assist(int v)  { if (v < 0) v = 0; if (v > 9) v = 9; s_catchup_assist = (uint32_t)v; }
/* Default NPC high-score table — raw PE .data bytes from 0x004643B8.
 * Loaded on fresh install (no Config.td5). Overwritten by td5_save_load_config
 * on success. 26 groups x 164 bytes = 4264 bytes. */
static const uint8_t k_npc_default_table[TD5_CONFIG_NPC_TABLE_SIZE] = {
    /* 0x0000 */ 0x00, 0x00, 0x00, 0x00, 0x46, 0x72, 0x61, 0x6E, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0010 */ 0x00, 0x00, 0x00, 0x00, 0xA4, 0x24, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xEA, 0x01, 0x00, 0x00,
    /* 0x0020 */ 0x0C, 0x03, 0x00, 0x00, 0x52, 0x61, 0x79, 0x6D, 0x6F, 0x6E, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0030 */ 0x00, 0x00, 0x00, 0x00, 0xB8, 0x24, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xCC, 0x01, 0x00, 0x00,
    /* 0x0040 */ 0xC6, 0x02, 0x00, 0x00, 0x42, 0x65, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0050 */ 0x00, 0x00, 0x00, 0x00, 0x4B, 0x25, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x9A, 0x01, 0x00, 0x00,
    /* 0x0060 */ 0x80, 0x02, 0x00, 0x00, 0x50, 0x61, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0070 */ 0x00, 0x00, 0x00, 0x00, 0x74, 0x25, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00,
    /* 0x0080 */ 0x26, 0x02, 0x00, 0x00, 0x4A, 0x65, 0x66, 0x66, 0x72, 0x65, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0090 */ 0x00, 0x00, 0x00, 0x00, 0x84, 0x25, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x72, 0x01, 0x00, 0x00,
    /* 0x00A0 */ 0xFE, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x65, 0x6F, 0x72, 0x67, 0x65, 0x00, 0x00,
    /* 0x00B0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x00C0 */ 0xF4, 0x01, 0x00, 0x00, 0x20, 0x03, 0x00, 0x00, 0x45, 0x64, 0x6E, 0x61, 0x00, 0x00, 0x00, 0x00,
    /* 0x00D0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x21, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    /* 0x00E0 */ 0xE0, 0x01, 0x00, 0x00, 0xD0, 0x02, 0x00, 0x00, 0x4B, 0x61, 0x74, 0x69, 0x65, 0x00, 0x00, 0x00,
    /* 0x00F0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x22, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    /* 0x0100 */ 0xA4, 0x01, 0x00, 0x00, 0x8A, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x74, 0x69, 0x6E, 0x00, 0x00,
    /* 0x0110 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6C, 0x22, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    /* 0x0120 */ 0x9A, 0x01, 0x00, 0x00, 0x44, 0x02, 0x00, 0x00, 0x47, 0x75, 0x74, 0x68, 0x72, 0x69, 0x65, 0x00,
    /* 0x0130 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA7, 0x22, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* 0x0140 */ 0x68, 0x01, 0x00, 0x00, 0x12, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x65, 0x64, 0x00,
    /* 0x0150 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDD, 0x1C, 0x00, 0x00,
    /* 0x0160 */ 0x03, 0x00, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x44, 0x6F, 0x75, 0x67,
    /* 0x0170 */ 0x61, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x00,
    /* 0x0180 */ 0x05, 0x00, 0x00, 0x00, 0xD6, 0x01, 0x00, 0x00, 0xC8, 0x02, 0x00, 0x00, 0x4A, 0x61, 0x63, 0x6B,
    /* 0x0190 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x1D, 0x00, 0x00,
    /* 0x01A0 */ 0x01, 0x00, 0x00, 0x00, 0xAE, 0x01, 0x00, 0x00, 0x86, 0x02, 0x00, 0x00, 0x4C, 0x65, 0x6E, 0x00,
    /* 0x01B0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x1D, 0x00, 0x00,
    /* 0x01C0 */ 0x02, 0x00, 0x00, 0x00, 0x9A, 0x01, 0x00, 0x00, 0x23, 0x02, 0x00, 0x00, 0x45, 0x6C, 0x76, 0x69,
    /* 0x01D0 */ 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x1D, 0x00, 0x00,
    /* 0x01E0 */ 0x07, 0x00, 0x00, 0x00, 0x5E, 0x01, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x01F0 */ 0x42, 0x61, 0x73, 0x69, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0200 */ 0x17, 0x26, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xE2, 0x01, 0x00, 0x00, 0x08, 0x03, 0x00, 0x00,
    /* 0x0210 */ 0x53, 0x79, 0x62, 0x69, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0220 */ 0x3D, 0x26, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xD4, 0x01, 0x00, 0x00, 0xCA, 0x02, 0x00, 0x00,
    /* 0x0230 */ 0x4D, 0x61, 0x6E, 0x75, 0x65, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0240 */ 0x73, 0x26, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00, 0x99, 0x02, 0x00, 0x00,
    /* 0x0250 */ 0x50, 0x6F, 0x6C, 0x6C, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0260 */ 0xB8, 0x26, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xA6, 0x01, 0x00, 0x00, 0x4B, 0x02, 0x00, 0x00,
    /* 0x0270 */ 0x4D, 0x61, 0x6A, 0x6F, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0280 */ 0x5C, 0x27, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x6D, 0x01, 0x00, 0x00, 0xFA, 0x01, 0x00, 0x00,
    /* 0x0290 */ 0x01, 0x00, 0x00, 0x00, 0x43, 0x65, 0x76, 0x69, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x02A0 */ 0x00, 0x00, 0x00, 0x00, 0x79, 0x0F, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xED, 0x01, 0x00, 0x00,
    /* 0x02B0 */ 0x04, 0x03, 0x00, 0x00, 0x44, 0x77, 0x61, 0x79, 0x6E, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x02C0 */ 0x00, 0x00, 0x00, 0x00, 0xC1, 0x0F, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xDB, 0x01, 0x00, 0x00,
    /* 0x02D0 */ 0xD4, 0x02, 0x00, 0x00, 0x4E, 0x69, 0x76, 0x65, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x02E0 */ 0x00, 0x00, 0x00, 0x00, 0x05, 0x10, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xB8, 0x01, 0x00, 0x00,
    /* 0x02F0 */ 0x9A, 0x02, 0x00, 0x00, 0x41, 0x6C, 0x61, 0x69, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0300 */ 0x00, 0x00, 0x00, 0x00, 0x39, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9B, 0x01, 0x00, 0x00,
    /* 0x0310 */ 0x4B, 0x02, 0x00, 0x00, 0x50, 0x61, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0320 */ 0x00, 0x00, 0x00, 0x00, 0x6A, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x6D, 0x01, 0x00, 0x00,
    /* 0x0330 */ 0x18, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x53, 0x61, 0x73, 0x63, 0x68, 0x61, 0x00, 0x00,
    /* 0x0340 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    /* 0x0350 */ 0xED, 0x01, 0x00, 0x00, 0x17, 0x03, 0x00, 0x00, 0x45, 0x6E, 0x65, 0x73, 0x63, 0x68, 0x00, 0x00,
    /* 0x0360 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9E, 0x0E, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* 0x0370 */ 0xD1, 0x01, 0x00, 0x00, 0xE7, 0x02, 0x00, 0x00, 0x47, 0x69, 0x6C, 0x62, 0x65, 0x72, 0x74, 0x00,
    /* 0x0380 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD9, 0x0E, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    /* 0x0390 */ 0xA5, 0x01, 0x00, 0x00, 0xB0, 0x02, 0x00, 0x00, 0x46, 0x72, 0x65, 0x64, 0x65, 0x72, 0x69, 0x63,
    /* 0x03A0 */ 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x0F, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    /* 0x03B0 */ 0x9A, 0x01, 0x00, 0x00, 0x2A, 0x02, 0x00, 0x00, 0x41, 0x6C, 0x62, 0x65, 0x72, 0x74, 0x00, 0x00,
    /* 0x03C0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x0F, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    /* 0x03D0 */ 0x88, 0x01, 0x00, 0x00, 0x0C, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4E, 0x6F, 0x72, 0x6D,
    /* 0x03E0 */ 0x61, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x0D, 0x00, 0x00,
    /* 0x03F0 */ 0x0E, 0x00, 0x00, 0x00, 0xE8, 0x01, 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x50, 0x68, 0x69, 0x6C,
    /* 0x0400 */ 0x69, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x0D, 0x00, 0x00,
    /* 0x0410 */ 0x0C, 0x00, 0x00, 0x00, 0xC6, 0x01, 0x00, 0x00, 0xCA, 0x02, 0x00, 0x00, 0x54, 0x6F, 0x6E, 0x79,
    /* 0x0420 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA9, 0x0D, 0x00, 0x00,
    /* 0x0430 */ 0x06, 0x00, 0x00, 0x00, 0xBD, 0x01, 0x00, 0x00, 0xB7, 0x02, 0x00, 0x00, 0x4D, 0x69, 0x63, 0x68,
    /* 0x0440 */ 0x61, 0x65, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE4, 0x0D, 0x00, 0x00,
    /* 0x0450 */ 0x00, 0x00, 0x00, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x37, 0x02, 0x00, 0x00, 0x47, 0x72, 0x61, 0x6E,
    /* 0x0460 */ 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0E, 0x00, 0x00,
    /* 0x0470 */ 0x03, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00, 0x36, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* 0x0480 */ 0x54, 0x65, 0x72, 0x72, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0490 */ 0x34, 0x0E, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xD1, 0x01, 0x00, 0x00, 0xFD, 0x02, 0x00, 0x00,
    /* 0x04A0 */ 0x50, 0x61, 0x74, 0x72, 0x69, 0x63, 0x69, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x04B0 */ 0x69, 0x0E, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xC8, 0x01, 0x00, 0x00, 0xDE, 0x02, 0x00, 0x00,
    /* 0x04C0 */ 0x43, 0x6F, 0x6E, 0x6E, 0x6F, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x04D0 */ 0x78, 0x0E, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x6F, 0x02, 0x00, 0x00,
    /* 0x04E0 */ 0x4B, 0x61, 0x74, 0x68, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x04F0 */ 0xD7, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x9F, 0x01, 0x00, 0x00, 0x1F, 0x02, 0x00, 0x00,
    /* 0x0500 */ 0x45, 0x72, 0x69, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0510 */ 0x0C, 0x0F, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x70, 0x01, 0x00, 0x00, 0x2A, 0x02, 0x00, 0x00,
    /* 0x0520 */ 0x00, 0x00, 0x00, 0x00, 0x45, 0x72, 0x6E, 0x69, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0530 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xDC, 0x01, 0x00, 0x00,
    /* 0x0540 */ 0x0A, 0x03, 0x00, 0x00, 0x54, 0x69, 0x66, 0x66, 0x61, 0x6E, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0550 */ 0x00, 0x00, 0x00, 0x00, 0x44, 0x26, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0xC8, 0x01, 0x00, 0x00,
    /* 0x0560 */ 0xF4, 0x02, 0x00, 0x00, 0x50, 0x65, 0x74, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0570 */ 0x00, 0x00, 0x00, 0x00, 0x73, 0x26, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00,
    /* 0x0580 */ 0x7A, 0x02, 0x00, 0x00, 0x53, 0x69, 0x6D, 0x6F, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0590 */ 0x00, 0x00, 0x00, 0x00, 0xA1, 0x26, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00,
    /* 0x05A0 */ 0x21, 0x02, 0x00, 0x00, 0x42, 0x69, 0x61, 0x6E, 0x63, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x05B0 */ 0x00, 0x00, 0x00, 0x00, 0xD4, 0x26, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00,
    /* 0x05C0 */ 0x1F, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x72, 0x65, 0x6E, 0x74, 0x00, 0x00, 0x00,
    /* 0x05D0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E, 0x23, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
    /* 0x05E0 */ 0xE7, 0x01, 0x00, 0x00, 0x13, 0x03, 0x00, 0x00, 0x4D, 0x61, 0x74, 0x74, 0x68, 0x65, 0x77, 0x00,
    /* 0x05F0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0x23, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00,
    /* 0x0600 */ 0xC8, 0x01, 0x00, 0x00, 0xF4, 0x02, 0x00, 0x00, 0x52, 0x61, 0x79, 0x6D, 0x6F, 0x6E, 0x64, 0x00,
    /* 0x0610 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7, 0x23, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    /* 0x0620 */ 0xBA, 0x01, 0x00, 0x00, 0x8E, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x69, 0x6C, 0x79, 0x6E, 0x00,
    /* 0x0630 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0x24, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    /* 0x0640 */ 0x9C, 0x01, 0x00, 0x00, 0x1F, 0x02, 0x00, 0x00, 0x43, 0x68, 0x72, 0x69, 0x73, 0x74, 0x69, 0x61,
    /* 0x0650 */ 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x24, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    /* 0x0660 */ 0x8D, 0x01, 0x00, 0x00, 0x0B, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x72, 0x65, 0x6E,
    /* 0x0670 */ 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x28, 0x00, 0x00,
    /* 0x0680 */ 0x0F, 0x00, 0x00, 0x00, 0xDC, 0x01, 0x00, 0x00, 0xFD, 0x02, 0x00, 0x00, 0x42, 0x65, 0x74, 0x74,
    /* 0x0690 */ 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7D, 0x28, 0x00, 0x00,
    /* 0x06A0 */ 0x0D, 0x00, 0x00, 0x00, 0xC8, 0x01, 0x00, 0x00, 0xE9, 0x02, 0x00, 0x00, 0x52, 0x6F, 0x62, 0x00,
    /* 0x06B0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x28, 0x00, 0x00,
    /* 0x06C0 */ 0x0B, 0x00, 0x00, 0x00, 0xBD, 0x01, 0x00, 0x00, 0x7A, 0x02, 0x00, 0x00, 0x41, 0x6E, 0x64, 0x72,
    /* 0x06D0 */ 0x65, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x29, 0x00, 0x00,
    /* 0x06E0 */ 0x0A, 0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x2A, 0x02, 0x00, 0x00, 0x57, 0x61, 0x72, 0x72,
    /* 0x06F0 */ 0x65, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0x29, 0x00, 0x00,
    /* 0x0700 */ 0x09, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00, 0x0B, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0710 */ 0x53, 0x6E, 0x61, 0x6B, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0720 */ 0x15, 0x1F, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0xD3, 0x01, 0x00, 0x00, 0xE7, 0x02, 0x00, 0x00,
    /* 0x0730 */ 0x42, 0x6F, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0740 */ 0x4C, 0x1F, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00, 0xDE, 0x02, 0x00, 0x00,
    /* 0x0750 */ 0x47, 0x61, 0x72, 0x65, 0x74, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0760 */ 0x8C, 0x1F, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00, 0x6F, 0x02, 0x00, 0x00,
    /* 0x0770 */ 0x43, 0x68, 0x72, 0x69, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0780 */ 0xFB, 0x1F, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xA4, 0x01, 0x00, 0x00, 0x1F, 0x02, 0x00, 0x00,
    /* 0x0790 */ 0x4D, 0x69, 0x6B, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x07A0 */ 0x2C, 0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00, 0x2C, 0x02, 0x00, 0x00,
    /* 0x07B0 */ 0x00, 0x00, 0x00, 0x00, 0x45, 0x6D, 0x6D, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x07C0 */ 0x00, 0x00, 0x00, 0x00, 0xAF, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF1, 0x01, 0x00, 0x00,
    /* 0x07D0 */ 0xF2, 0x02, 0x00, 0x00, 0x41, 0x72, 0x6E, 0x6F, 0x6C, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x07E0 */ 0x00, 0x00, 0x00, 0x00, 0xF0, 0x24, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xC6, 0x01, 0x00, 0x00,
    /* 0x07F0 */ 0xDE, 0x02, 0x00, 0x00, 0x44, 0x61, 0x76, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0800 */ 0x00, 0x00, 0x00, 0x00, 0x25, 0x25, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00,
    /* 0x0810 */ 0x85, 0x02, 0x00, 0x00, 0x43, 0x68, 0x61, 0x72, 0x6D, 0x61, 0x69, 0x6E, 0x65, 0x00, 0x00, 0x00,
    /* 0x0820 */ 0x00, 0x00, 0x00, 0x00, 0xAD, 0x25, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00,
    /* 0x0830 */ 0x16, 0x02, 0x00, 0x00, 0x48, 0x6F, 0x6C, 0x6C, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0840 */ 0x00, 0x00, 0x00, 0x00, 0xE8, 0x25, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x62, 0x01, 0x00, 0x00,
    /* 0x0850 */ 0x0B, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x61, 0x6D, 0x6F, 0x6E, 0x00, 0x00, 0x00,
    /* 0x0860 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x1C, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    /* 0x0870 */ 0xC6, 0x01, 0x00, 0x00, 0xFD, 0x02, 0x00, 0x00, 0x4C, 0x75, 0x63, 0x79, 0x00, 0x00, 0x00, 0x00,
    /* 0x0880 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x1D, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    /* 0x0890 */ 0xA7, 0x01, 0x00, 0x00, 0xDE, 0x02, 0x00, 0x00, 0x4B, 0x65, 0x76, 0x69, 0x6E, 0x00, 0x00, 0x00,
    /* 0x08A0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x71, 0x1D, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    /* 0x08B0 */ 0xA6, 0x01, 0x00, 0x00, 0x8E, 0x02, 0x00, 0x00, 0x45, 0x6C, 0x69, 0x73, 0x73, 0x61, 0x00, 0x00,
    /* 0x08C0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9B, 0x1D, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00,
    /* 0x08D0 */ 0x9C, 0x01, 0x00, 0x00, 0x0B, 0x02, 0x00, 0x00, 0x4A, 0x6F, 0x64, 0x69, 0x65, 0x00, 0x00, 0x00,
    /* 0x08E0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB3, 0x1D, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00,
    /* 0x08F0 */ 0x82, 0x01, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x61, 0x6E, 0x69,
    /* 0x0900 */ 0x65, 0x6C, 0x6C, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x1C, 0x00, 0x00,
    /* 0x0910 */ 0x02, 0x00, 0x00, 0x00, 0xE7, 0x01, 0x00, 0x00, 0xF2, 0x02, 0x00, 0x00, 0x4A, 0x69, 0x6D, 0x00,
    /* 0x0920 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x1C, 0x00, 0x00,
    /* 0x0930 */ 0x04, 0x00, 0x00, 0x00, 0xC8, 0x01, 0x00, 0x00, 0xDE, 0x02, 0x00, 0x00, 0x4E, 0x61, 0x72, 0x65,
    /* 0x0940 */ 0x6C, 0x6C, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x1C, 0x00, 0x00,
    /* 0x0950 */ 0x07, 0x00, 0x00, 0x00, 0xBD, 0x01, 0x00, 0x00, 0xA3, 0x02, 0x00, 0x00, 0x4D, 0x69, 0x63, 0x6B,
    /* 0x0960 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x1C, 0x00, 0x00,
    /* 0x0970 */ 0x09, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00, 0x21, 0x02, 0x00, 0x00, 0x50, 0x6F, 0x70, 0x70,
    /* 0x0980 */ 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBF, 0x1C, 0x00, 0x00,
    /* 0x0990 */ 0x0A, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x09A0 */ 0x53, 0x61, 0x72, 0x61, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x09B0 */ 0x65, 0x1C, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xED, 0x01, 0x00, 0x00, 0x08, 0x03, 0x00, 0x00,
    /* 0x09C0 */ 0x4A, 0x6F, 0x68, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x09D0 */ 0x83, 0x1C, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xDC, 0x01, 0x00, 0x00, 0xE9, 0x02, 0x00, 0x00,
    /* 0x09E0 */ 0x4B, 0x79, 0x6C, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x09F0 */ 0xA4, 0x1C, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xBC, 0x01, 0x00, 0x00, 0x7A, 0x02, 0x00, 0x00,
    /* 0x0A00 */ 0x41, 0x72, 0x6E, 0x6F, 0x6C, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0A10 */ 0xB0, 0x1C, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00, 0x35, 0x02, 0x00, 0x00,
    /* 0x0A20 */ 0x44, 0x65, 0x6E, 0x6E, 0x69, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0A30 */ 0xBF, 0x1C, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x8B, 0x01, 0x00, 0x00, 0x16, 0x02, 0x00, 0x00,
    /* 0x0A40 */ 0x01, 0x00, 0x00, 0x00, 0x53, 0x6C, 0x61, 0x64, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0A50 */ 0x00, 0x00, 0x00, 0x00, 0x19, 0x0E, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xD1, 0x01, 0x00, 0x00,
    /* 0x0A60 */ 0x13, 0x03, 0x00, 0x00, 0x52, 0x6F, 0x64, 0x6E, 0x65, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0A70 */ 0x00, 0x00, 0x00, 0x00, 0x80, 0x0E, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0xBD, 0x01, 0x00, 0x00,
    /* 0x0A80 */ 0xF4, 0x02, 0x00, 0x00, 0x41, 0x6C, 0x61, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0A90 */ 0x00, 0x00, 0x00, 0x00, 0xCB, 0x0E, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00,
    /* 0x0AA0 */ 0x8E, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x69, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0AB0 */ 0x00, 0x00, 0x00, 0x00, 0x19, 0x0F, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x9C, 0x01, 0x00, 0x00,
    /* 0x0AC0 */ 0x1F, 0x02, 0x00, 0x00, 0x42, 0x65, 0x6C, 0x69, 0x6E, 0x64, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0AD0 */ 0x00, 0x00, 0x00, 0x00, 0x3E, 0x0F, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x77, 0x01, 0x00, 0x00,
    /* 0x0AE0 */ 0x14, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x43, 0x72, 0x61, 0x69, 0x67, 0x00, 0x00, 0x00,
    /* 0x0AF0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9B, 0x0E, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00,
    /* 0x0B00 */ 0xE7, 0x01, 0x00, 0x00, 0x12, 0x03, 0x00, 0x00, 0x4E, 0x6F, 0x72, 0x6D, 0x61, 0x6E, 0x00, 0x00,
    /* 0x0B10 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCD, 0x0E, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    /* 0x0B20 */ 0xD3, 0x01, 0x00, 0x00, 0xE0, 0x02, 0x00, 0x00, 0x48, 0x61, 0x74, 0x74, 0x79, 0x00, 0x00, 0x00,
    /* 0x0B30 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0x0E, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00,
    /* 0x0B40 */ 0xC8, 0x01, 0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x44, 0x61, 0x6E, 0x6E, 0x79, 0x00, 0x00, 0x00,
    /* 0x0B50 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x0F, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* 0x0B60 */ 0xBB, 0x01, 0x00, 0x00, 0x2A, 0x02, 0x00, 0x00, 0x43, 0x68, 0x72, 0x69, 0x73, 0x00, 0x00, 0x00,
    /* 0x0B70 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E, 0x0F, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    /* 0x0B80 */ 0x85, 0x01, 0x00, 0x00, 0x1F, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x52, 0x6F, 0x62, 0x00,
    /* 0x0B90 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA9, 0x0C, 0x00, 0x00,
    /* 0x0BA0 */ 0x12, 0x00, 0x00, 0x00, 0xD1, 0x01, 0x00, 0x00, 0xF2, 0x02, 0x00, 0x00, 0x44, 0x6F, 0x75, 0x67,
    /* 0x0BB0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x0C, 0x00, 0x00,
    /* 0x0BC0 */ 0x0F, 0x00, 0x00, 0x00, 0xBD, 0x01, 0x00, 0x00, 0xDE, 0x02, 0x00, 0x00, 0x53, 0x61, 0x6D, 0x75,
    /* 0x0BD0 */ 0x65, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0D, 0x00, 0x00,
    /* 0x0BE0 */ 0x11, 0x00, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x64, 0x02, 0x00, 0x00, 0x4A, 0x6F, 0x68, 0x6E,
    /* 0x0BF0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x0D, 0x00, 0x00,
    /* 0x0C00 */ 0x13, 0x00, 0x00, 0x00, 0x9C, 0x01, 0x00, 0x00, 0x35, 0x02, 0x00, 0x00, 0x48, 0x61, 0x72, 0x76,
    /* 0x0C10 */ 0x65, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEF, 0x0D, 0x00, 0x00,
    /* 0x0C20 */ 0x0C, 0x00, 0x00, 0x00, 0x6D, 0x01, 0x00, 0x00, 0x0B, 0x02, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    /* 0x0C30 */ 0x54, 0x69, 0x6D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0C40 */ 0x40, 0xFB, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00, 0x09, 0x01, 0x00, 0x00, 0x8E, 0x02, 0x00, 0x00,
    /* 0x0C50 */ 0x55, 0x6D, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0C60 */ 0x98, 0x16, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x22, 0x02, 0x00, 0x00,
    /* 0x0C70 */ 0x45, 0x72, 0x69, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0C80 */ 0x70, 0x70, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x00, 0x00, 0x00, 0x1C, 0x02, 0x00, 0x00,
    /* 0x0C90 */ 0x43, 0x68, 0x72, 0x69, 0x73, 0x74, 0x6F, 0x70, 0x68, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0CA0 */ 0x48, 0xCA, 0x06, 0x00, 0x07, 0x00, 0x00, 0x00, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    /* 0x0CB0 */ 0x42, 0x72, 0x75, 0x63, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0CC0 */ 0xB0, 0x89, 0x07, 0x00, 0x06, 0x00, 0x00, 0x00, 0xD4, 0x00, 0x00, 0x00, 0xE7, 0x01, 0x00, 0x00,
    /* 0x0CD0 */ 0x02, 0x00, 0x00, 0x00, 0x41, 0x6D, 0x61, 0x6E, 0x64, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0CE0 */ 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xE7, 0x01, 0x00, 0x00,
    /* 0x0CF0 */ 0xF2, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x69, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0D00 */ 0x00, 0x00, 0x00, 0x00, 0x5A, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xC6, 0x01, 0x00, 0x00,
    /* 0x0D10 */ 0xF0, 0x02, 0x00, 0x00, 0x56, 0x69, 0x6E, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0D20 */ 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00,
    /* 0x0D30 */ 0x83, 0x02, 0x00, 0x00, 0x52, 0x6F, 0x73, 0x61, 0x6E, 0x6E, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0D40 */ 0x00, 0x00, 0x00, 0x00, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x01, 0x00, 0x00,
    /* 0x0D50 */ 0x2A, 0x02, 0x00, 0x00, 0x51, 0x75, 0x65, 0x6E, 0x74, 0x69, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0D60 */ 0x00, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x57, 0x01, 0x00, 0x00,
    /* 0x0D70 */ 0x1F, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x69, 0x6E, 0x63, 0x65, 0x6E, 0x74, 0x00,
    /* 0x0D80 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x54, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* 0x0D90 */ 0xDC, 0x01, 0x00, 0x00, 0x0A, 0x03, 0x00, 0x00, 0x4D, 0x69, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0DA0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x58, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
    /* 0x0DB0 */ 0xC8, 0x01, 0x00, 0x00, 0xF4, 0x02, 0x00, 0x00, 0x42, 0x75, 0x74, 0x63, 0x68, 0x00, 0x00, 0x00,
    /* 0x0DC0 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x5D, 0x01, 0x00, 0x0C, 0x00, 0x00, 0x00,
    /* 0x0DD0 */ 0xBD, 0x01, 0x00, 0x00, 0xE7, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x63, 0x65, 0x6C, 0x6C, 0x75,
    /* 0x0DE0 */ 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8D, 0x62, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
    /* 0x0DF0 */ 0xB2, 0x01, 0x00, 0x00, 0xC8, 0x02, 0x00, 0x00, 0x47, 0x72, 0x65, 0x67, 0x00, 0x00, 0x00, 0x00,
    /* 0x0E00 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x67, 0x01, 0x00, 0x0F, 0x00, 0x00, 0x00,
    /* 0x0E10 */ 0x8E, 0x01, 0x00, 0x00, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x6F, 0x68, 0x6E,
    /* 0x0E20 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xB8, 0x00, 0x00,
    /* 0x0E30 */ 0x02, 0x00, 0x00, 0x00, 0xDE, 0x01, 0x00, 0x00, 0xF4, 0x02, 0x00, 0x00, 0x50, 0x61, 0x75, 0x6C,
    /* 0x0E40 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E, 0xBD, 0x00, 0x00,
    /* 0x0E50 */ 0x08, 0x00, 0x00, 0x00, 0xC7, 0x01, 0x00, 0x00, 0xE7, 0x02, 0x00, 0x00, 0x52, 0x69, 0x6E, 0x67,
    /* 0x0E60 */ 0x6F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB3, 0xC1, 0x00, 0x00,
    /* 0x0E70 */ 0x04, 0x00, 0x00, 0x00, 0xC6, 0x01, 0x00, 0x00, 0x71, 0x02, 0x00, 0x00, 0x47, 0x65, 0x6F, 0x72,
    /* 0x0E80 */ 0x67, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xC8, 0x00, 0x00,
    /* 0x0E90 */ 0x0A, 0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x28, 0x02, 0x00, 0x00, 0x57, 0x69, 0x6C, 0x66,
    /* 0x0EA0 */ 0x72, 0x65, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0xCB, 0x00, 0x00,
    /* 0x0EB0 */ 0x0B, 0x00, 0x00, 0x00, 0x7A, 0x01, 0x00, 0x00, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0EC0 */ 0x4A, 0x61, 0x6D, 0x65, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0ED0 */ 0x16, 0xDD, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0xE6, 0x01, 0x00, 0x00, 0x05, 0x03, 0x00, 0x00,
    /* 0x0EE0 */ 0x53, 0x63, 0x6F, 0x74, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0EF0 */ 0x1F, 0xE5, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0xD0, 0x01, 0x00, 0x00, 0xFB, 0x02, 0x00, 0x00,
    /* 0x0F00 */ 0x53, 0x74, 0x65, 0x70, 0x68, 0x65, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0F10 */ 0x4A, 0xEA, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xC4, 0x01, 0x00, 0x00, 0xA1, 0x02, 0x00, 0x00,
    /* 0x0F20 */ 0x53, 0x68, 0x69, 0x72, 0x6C, 0x65, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0F30 */ 0x40, 0xED, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xB9, 0x01, 0x00, 0x00, 0x28, 0x02, 0x00, 0x00,
    /* 0x0F40 */ 0x44, 0x75, 0x6B, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0F50 */ 0xB7, 0xF1, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x6A, 0x01, 0x00, 0x00, 0x1D, 0x02, 0x00, 0x00,
    /* 0x0F60 */ 0x00, 0x00, 0x00, 0x00, 0x53, 0x74, 0x65, 0x76, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0F70 */ 0x00, 0x00, 0x00, 0x00, 0xA0, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE9, 0x01, 0x00, 0x00,
    /* 0x0F80 */ 0xFB, 0x02, 0x00, 0x00, 0x42, 0x75, 0x74, 0x63, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0F90 */ 0x00, 0x00, 0x00, 0x00, 0xD2, 0x32, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xC5, 0x01, 0x00, 0x00,
    /* 0x0FA0 */ 0xF0, 0x02, 0x00, 0x00, 0x4A, 0x6F, 0x61, 0x6E, 0x6E, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0FB0 */ 0x00, 0x00, 0x00, 0x00, 0x68, 0x38, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0xB2, 0x01, 0x00, 0x00,
    /* 0x0FC0 */ 0xA5, 0x02, 0x00, 0x00, 0x50, 0x68, 0x69, 0x6C, 0x69, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0FD0 */ 0x00, 0x00, 0x00, 0x00, 0x8C, 0x3E, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0xAF, 0x01, 0x00, 0x00,
    /* 0x0FE0 */ 0x2F, 0x02, 0x00, 0x00, 0x53, 0x75, 0x73, 0x61, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x0FF0 */ 0x00, 0x00, 0x00, 0x00, 0xC8, 0x41, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x70, 0x01, 0x00, 0x00,
    /* 0x1000 */ 0x21, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4E, 0x69, 0x63, 0x6F, 0x6C, 0x65, 0x00, 0x00,
    /* 0x1010 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xF7, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    /* 0x1020 */ 0xEE, 0x01, 0x00, 0x00, 0xFC, 0x02, 0x00, 0x00, 0x4D, 0x65, 0x6C, 0x61, 0x6E, 0x69, 0x65, 0x00,
    /* 0x1030 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0xF0, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    /* 0x1040 */ 0xDB, 0x01, 0x00, 0x00, 0xF1, 0x02, 0x00, 0x00, 0x4E, 0x61, 0x74, 0x61, 0x6C, 0x69, 0x65, 0x00,
    /* 0x1050 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xEE, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    /* 0x1060 */ 0xBA, 0x01, 0x00, 0x00, 0x97, 0x02, 0x00, 0x00, 0x53, 0x68, 0x61, 0x7A, 0x6E, 0x61, 0x79, 0x00,
    /* 0x1070 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xE9, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* 0x1080 */ 0xB0, 0x01, 0x00, 0x00, 0x1E, 0x02, 0x00, 0x00, 0x4D, 0x61, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x00,
    /* 0x1090 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAD, 0xE5, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    /* 0x10A0 */ 0x82, 0x01, 0x00, 0x00, 0x16, 0x02, 0x00, 0x00
};
/* ========================================================================
 * Celebrity high-score names (port enhancement)
 *
 * Replaces the original generic first names (Frank, Raymond, Ben…) from
 * k_npc_default_table with well-known celebrity names.  Applied once at
 * startup for every NPC slot whose saved name still matches the original
 * binary default — genuine player-set names are never touched.
 *
 * 130 entries = TD5_CONFIG_NPC_GROUPS(26) × 5 entries per group.
 * Groups 0-19 = race tracks; 20-25 = cup tracks.
 * ======================================================================== */
/* Full celebrity names (<=31 chars to fit TD5_NpcEntryExt.full_name[32]). The
 * short first name is still stored in the 15-char entry.name[16] (for legacy
 * Config.td5 round-trip); the full name lives in the parallel extension and is
 * what the High Scores table displays. */
static const char * const k_celebrity_names[TD5_CONFIG_NPC_GROUPS * 5] = {
    /* 0 Moscow */      "Michael Schumacher", "Lewis Hamilton",     "Ayrton Senna",         "Damon Hill",          "Mika Hakkinen",
    /* 1 Edinburgh */   "Fernando Alonso",    "Sebastian Vettel",   "David Coulthard",      "Rubens Barrichello",  "Nigel Mansell",
    /* 2 Sydney */      "Alain Prost",        "Jackie Stewart",     "James Hunt",           "Niki Lauda",          "Emerson Fittipaldi",
    /* 3 Blue Ridge */  "Tom Cruise",         "Brad Pitt",          "Leonardo DiCaprio",    "Julia Roberts",       "Denzel Washington",
    /* 4 Jarash */      "Arnold Schwarzenegger", "Sylvester Stallone", "Bruce Willis",      "Harrison Ford",       "Will Smith",
    /* 5 Newcastle */   "Robert De Niro",     "Jack Nicholson",     "Meryl Streep",         "Halle Berry",         "Keanu Reeves",
    /* 6 Maui */        "Madonna Ciccone",    "Beyonce Knowles",    "Eminem Mathers",       "Britney Spears",      "Whitney Houston",
    /* 7 Courmayeur */  "Celine Dion",        "Elton John",         "Bono Hewson",          "Sting Sumner",        "Tina Turner",
    /* 8 Honolulu */    "Diana Ross",         "Aretha Franklin",    "Elvis Presley",        "Prince Nelson",       "Mick Jagger",
    /* 9 Tokyo */       "Tiger Woods",        "Muhammad Ali",       "Carl Lewis",           "Jesse Owens",         "Andre Agassi",
    /* 10 Keswick */    "Steffi Graf",        "Martina Navratilova","Kobe Bryant",          "Shaquille O'Neal",    "Magic Johnson",
    /* 11 San Fran */   "Wayne Gretzky",      "Mario Lemieux",      "Pele Nascimento",      "Diego Maradona",      "Zinedine Zidane",
    /* 12 Bern */       "Oprah Winfrey",      "Angelina Jolie",     "Jennifer Aniston",     "Gwyneth Paltrow",     "Reese Witherspoon",
    /* 13 Kyoto */      "Charlize Theron",    "Hilary Swank",       "Cameron Diaz",         "Drew Barrymore",      "Winona Ryder",
    /* 14 Washington */ "Salma Hayek",        "Penelope Cruz",      "Monica Bellucci",      "Jodie Foster",        "Jessica Alba",
    /* 15 Munich */     "Natalie Portman",    "Scarlett Johansson", "Keira Knightley",      "Cate Blanchett",      "Nicole Kidman",
    /* 16 Cheddar */    "Colin McRae",        "Sebastien Loeb",     "Tommi Makinen",        "Carlos Sainz",        "Petter Solberg",
    /* 17 Montego */    "Usain Bolt",         "Florence Joyner",    "Venus Williams",       "Serena Williams",     "Roger Federer",
    /* 18 Bez */        "Pete Sampras",       "Lleyton Hewitt",     "Marat Safin",          "Andy Roddick",        "Rafael Nadal",
    /* 19 Drag Strip */ "Freddie Mercury",    "Keith Richards",     "Paul McCartney",       "George Harrison",     "Ringo Starr",
    /* 20 Cup 1 */      "Ronaldo Nazario",    "Thierry Henry",      "Patrick Vieira",       "Oliver Kahn",         "Raul Gonzalez",
    /* 21 Cup 2 */      "Naomi Campbell",     "Claudia Schiffer",   "Cindy Crawford",       "Elle Macpherson",     "Karlie Kloss",
    /* 22 Cup 3 */      "Russell Crowe",      "Hugh Jackman",       "Geoffrey Rush",        "Rachel Weisz",        "Sarah Connor",
    /* 23 Cup 4 */      "Orlando Bloom",      "Viggo Mortensen",    "Elijah Wood",          "Ian McKellen",        "Sean Bean",
    /* 24 Cup 5 */      "Uma Thurman",        "Julianne Moore",     "Helen Mirren",         "Judi Dench",          "Lily Collins",
    /* 25 Cup 6 */      "Morgan Freeman",     "Samuel Jackson",     "Laurence Fishburne",   "Chiwetel Ejiofor",    "Idris Elba",
};

/* Extract the original default name for slot (group g, entry e).
 * The k_npc_default_table is a flat byte array; each group is
 * TD5_CONFIG_NPC_GROUP_SIZE bytes; within a group the header is 4 bytes
 * and each TD5_NpcEntry is 32 bytes with name[16] at offset 0. */
static void npc_default_name(int g, int e, char *out)
{
    int offset = g * TD5_CONFIG_NPC_GROUP_SIZE + 4 /* header */ + e * 32;
    const char *src = (const char *)(k_npc_default_table + offset);
    int i;
    for (i = 0; i < 15 && src[i]; i++) out[i] = src[i];
    out[i] = '\0';
}

/* -----------------------------------------------------------------------
 * WinHTTP helper: fetch first names from randomuser.me and fill names[][].
 * Returns the number of names successfully parsed (0 on any failure).
 * Only called when g_td5.ini.celebrity_names_api == 1.
 * Timeout: 4 s per phase so startup stall is bounded.
 * ----------------------------------------------------------------------- */
#define API_BUF_CAP (48 * 1024)

static int npc_fetch_api_names(char names[][16], int max_names)
{
    HINTERNET hSess = NULL, hConn = NULL, hReq = NULL;
    char *body = NULL;
    DWORD body_len = 0, avail = 0, read_n = 0;
    int count = 0;

    hSess = WinHttpOpen(L"TD5RE/1.0",
                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) {
        TD5_LOG_W(LOG_TAG, "celebrity API: WinHttpOpen failed %lu", GetLastError());
        goto done;
    }
    /* 4-second timeouts per phase */
    {
        DWORD t = 4000;
        WinHttpSetOption(hSess, WINHTTP_OPTION_CONNECT_TIMEOUT,  &t, sizeof t);
        WinHttpSetOption(hSess, WINHTTP_OPTION_SEND_TIMEOUT,     &t, sizeof t);
        WinHttpSetOption(hSess, WINHTTP_OPTION_RECEIVE_TIMEOUT,  &t, sizeof t);
    }

    hConn = WinHttpConnect(hSess, L"randomuser.me",
                           INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConn) {
        TD5_LOG_W(LOG_TAG, "celebrity API: WinHttpConnect failed %lu", GetLastError());
        goto done;
    }

    hReq = WinHttpOpenRequest(hConn, L"GET",
                              L"/api/?results=130&nat=us,gb&inc=name",
                              NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) {
        TD5_LOG_W(LOG_TAG, "celebrity API: WinHttpOpenRequest failed %lu", GetLastError());
        goto done;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, NULL)) {
        TD5_LOG_W(LOG_TAG, "celebrity API: request/response failed %lu", GetLastError());
        goto done;
    }

    body = (char *)malloc(API_BUF_CAP + 1);
    if (!body) goto done;

    while (body_len < API_BUF_CAP) {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        if (body_len + avail > API_BUF_CAP) avail = API_BUF_CAP - body_len;
        read_n = 0;
        if (!WinHttpReadData(hReq, body + body_len, avail, &read_n)) break;
        body_len += read_n;
    }
    body[body_len] = '\0';

    /* Parse: scan for "first":"<value>" tokens */
    {
        const char *key = "\"first\":\"";
        int klen = (int)strlen(key);
        const char *p = body;
        while (count < max_names) {
            const char *found = strstr(p, key);
            if (!found) break;
            found += klen;
            int n = 0;
            while (n < 15 && found[n] && found[n] != '"') n++;
            if (n > 0) {
                memset(names[count], 0, 16);
                memcpy(names[count], found, (size_t)n);
                count++;
            }
            p = found + n + 1;
        }
    }

    TD5_LOG_I(LOG_TAG, "celebrity API: fetched %d names from randomuser.me", count);

done:
    if (body)  free(body);
    if (hReq)  WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    return count;
}

static uint8_t  s_npc_group_table[TD5_CONFIG_NPC_TABLE_SIZE]; /* 0x4643B8 */
/* TD5RE parallel extensions to s_npc_group_table entries (full names >15 chars,
 * collisions + air time for the race-results-parity High Scores columns). Same
 * [group][entry] indexing; kept in lockstep by td5_save_npc_record_insert. */
static TD5_NpcEntryExt s_npc_ext[TD5_CONFIG_NPC_GROUPS][5];
static uint32_t s_cup_tier;                                   /* 0x4962A8 */
static uint32_t s_max_unlocked_car;                           /* 0x463E0C */
static uint8_t  s_all_cars_unlocked;                          /* 0x4962B0 */
static uint8_t  s_track_locks[TD5_CONFIG_NUM_TRACKS];         /* 0x4668B0 */
static uint8_t  s_car_locks[TD5_CONFIG_NUM_CARS];             /* 0x463E4C */
static uint8_t  s_cheat_flags[TD5_CONFIG_NUM_CHEATS];         /* 0x4A2C9C */

/* -- Globals that feed into CupData.td5 -- */
static uint32_t s_selected_game_type;                         /* 0x49635C */
static uint32_t s_race_within_series;                         /* 0x494BB8 */
static uint32_t s_npc_group_index;                            /* 0x4A2C90 */
static uint32_t s_track_opponent_state;                       /* 0x490BA8 */
static uint32_t s_race_rule_variant;                          /* 0x4AAF70 */
static uint32_t s_time_trial_enabled;                         /* 0x4AAF6C */
static uint32_t s_wanted_enabled;                             /* 0x4AAF68 */
static uint32_t s_difficulty_tier;                            /* 0x463210 */
static uint32_t s_checkpoint_mode;                            /* 0x4B0FA8 */
static uint32_t s_traffic_enabled;                            /* 0x4AAD8C */
static uint32_t s_special_encounter;                          /* 0x46320C */
static uint32_t s_circuit_lap_count;                          /* 0x466E8C */
static uint32_t s_race_schedule[0x1E];                        /* 0x497250 */
static uint32_t s_race_results[0x1E];                         /* 0x48D988 */
static uint32_t s_actor_table[0xC5C];                         /* 0x4AB108 */
static uint32_t s_slot_state[6];                              /* 0x4AADF4 */
static uint32_t s_masters_schedule_base;                      /* 0x48F30C */
static uint32_t s_p1_cup_schedule_index;                            /* 0x48F314 */
static uint32_t s_p2_cup_schedule_index;                            /* 0x48F310 */
static uint8_t  s_p1_selected_cup_id;                             /* 0x48F31A */
static uint16_t s_p1_cup_completion_bitmask;                             /* 0x48F318 */
static uint32_t s_p1_masters_unlock_bitmask;                            /* 0x48F328 */
static uint32_t s_masters_encounter_flags;                    /* 0x48F324 */
static uint32_t s_p2_masters_unlock_bitmask;                            /* 0x48F32C */
static uint16_t s_p2_cup_completion_bitmask;                             /* 0x48F330 */
static uint8_t  s_p2_cup_lock_flag;                             /* 0x48F332 */

/* Cross-references restored from within the schedule region. */
static uint32_t s_cup_progress_marker;                        /* 0x48F364 */
static uint32_t s_cup_cross_ref_1;                            /* 0x48F368 */
static uint32_t s_cup_cross_ref_2;                            /* 0x48F370 */
static uint32_t s_cup_cross_ref_3;                            /* 0x48F378 */

/* -- Player profiles (port enhancement #11) --
 * Stored in td5re_progress.ini [Profiles]; loaded lazily on first access,
 * rewritten on every save/delete. s_profiles_loaded gates the lazy read so a
 * single missing/empty store yields count 0 without re-reading every call. */
static TD5_Profile s_profiles[TD5_MAX_PROFILES];
static int         s_profile_count;
static int         s_profiles_loaded;

/* -- TD6 high-score records (port enhancement #2b) --
 * One TD5_NpcGroup-shaped record table per TD6 level, holding ONLY genuine
 * player runs (no seed/placeholder names). Stored in td5re_progress.ini
 * [TD6Records.LevelNN]; loaded lazily on first access, rewritten on every
 * insert. A group with header == -1 means "no records yet" (distinct from a
 * cleared time group whose header is 0). TD6 levels number up to ~39 (the
 * highest level<NN>.zip); 48 gives headroom. */
#define TD5_MAX_TD6_RECORD_LEVELS 48
static TD5_NpcGroup s_td6_records[TD5_MAX_TD6_RECORD_LEVELS];
/* TD5RE parallel extension to s_td6_records entries (see s_npc_ext). */
static TD5_NpcEntryExt s_td6_ext[TD5_MAX_TD6_RECORD_LEVELS][5];
static int          s_td6_records_loaded;

/* Apply celebrity (or API-fetched) names to NPC slots that still hold the
 * original binary defaults.  Called once at the end of td5_save_init() after
 * all save data has been loaded so player-set names are already in place. */
static void npc_apply_celebrity_names(char api_names[][16], int api_count)
{
    int g, e, k;
    TD5_NpcGroup *groups = (TD5_NpcGroup *)s_npc_group_table;
    int applied = 0, retimed = 0;

    for (g = 0; g < TD5_CONFIG_NPC_GROUPS; g++) {
        TD5_NpcGroup *grp = &groups[g];

        /* Precompute this group's five original binary-default names (Frank/Ben/…),
         * their celebrity FULL names, and the celebrity first tokens. Each entry is
         * matched against ALL five so a SHIFTED seed (pushed down when a genuine
         * record was inserted at a better rank) is still recognised, and the seed
         * keeps its celebrity IDENTITY regardless of its current row. */
        char        def[5][16];
        char        cshort[5][16];
        const char *cel[5];
        for (k = 0; k < 5; k++) {
            int idxk = g * 5 + k;
            cel[k] = (api_names && idxk < api_count && api_names[idxk][0])
                     ? api_names[idxk] : k_celebrity_names[idxk];
            int si = 0;
            for (const char *p = cel[k]; *p && *p != ' ' && si < 15; ++p) cshort[k][si++] = *p;
            cshort[k][si] = '\0';
            npc_default_name(g, k, def[k]);
        }

        /* Track whether the WHOLE group is still unraced SEED data (never a genuine
         * player run). Only then do we normalize its seed times — a group holding a
         * real record must keep its sorted times, or clamping seed rows could
         * disorder the table. */
        int all_seed = 1;

        for (e = 0; e < 5; e++) {
            const char *nm = grp->entries[e].name;

            /* Which seed identity (if any) does this entry hold? Prefer a binary-
             * default match (Frank/Ben/… -> replace with the celebrity), else a
             * celebrity first-name match (pre-full-name save -> just fill the ext). */
            int def_k = -1, cel_k = -1;
            for (k = 0; k < 5; k++)
                if (def[k][0] && strncmp(nm, def[k], 15) == 0) { def_k = k; break; }
            if (def_k < 0)
                for (k = 0; k < 5; k++)
                    if (cshort[k][0] && strncmp(nm, cshort[k], 15) == 0) { cel_k = k; break; }

            if (def_k >= 0) {
                /* Seed at its factory (possibly shifted) name: set the celebrity
                 * short name in the 15-char field + the full name in the extension. */
                memset(grp->entries[e].name, 0, 16);
                strncpy(grp->entries[e].name, cshort[def_k], 15);
                memset(s_npc_ext[g][e].full_name, 0, sizeof(s_npc_ext[g][e].full_name));
                strncpy(s_npc_ext[g][e].full_name, cel[def_k], sizeof(s_npc_ext[g][e].full_name) - 1);
                applied++;
            } else if (cel_k >= 0 && s_npc_ext[g][e].full_name[0] == '\0') {
                /* Pre-full-name save (celebrity FIRST name only, no extension yet):
                 * fill in the full name without touching the stored score/name. */
                memset(s_npc_ext[g][e].full_name, 0, sizeof(s_npc_ext[g][e].full_name));
                strncpy(s_npc_ext[g][e].full_name, cel[cel_k], sizeof(s_npc_ext[g][e].full_name) - 1);
                applied++;
            } else {
                all_seed = 0;   /* genuine player entry */
            }
        }

        /* Normalize default best times to 5-10 min for fully-unraced TIME groups.
         * Header 0/4 = race TIME (ticks @30fps): 5:00=9000 .. 9:00=16200, plus a
         * small group-uniform offset so tracks don't all read identically while
         * staying inside [9000,18000] = [5,10] min. Order is preserved (offset is
         * the same for all 5 rows). LAP(1)/POINTS(2) groups are left untouched. */
        if (all_seed) {
            int htype = grp->header & 0xFF;
            if (htype == 0 || htype == 4) {
                int jitter = (g * 37) % 600;   /* 0..599 ticks, group-uniform */
                for (e = 0; e < 5; e++)
                    grp->entries[e].score = 9000 + e * 1800 + jitter;
                retimed++;
            }
        }
    }

    TD5_LOG_I(LOG_TAG,
              "celebrity names: applied full names to %d/%d NPC slots; normalized default times for %d group(s)",
              applied, TD5_CONFIG_NPC_GROUPS * 5, retimed);
}

/* ========================================================================
 * Initialization / Shutdown
 * ======================================================================== */

int td5_save_init(void)
{
    int i;

    memset(s_config_buf, 0, sizeof(s_config_buf));
    memset(s_cup_buf, 0, sizeof(s_cup_buf));
    s_cup_buf_size = 0;
    memcpy(s_npc_group_table, k_npc_default_table, sizeof(s_npc_group_table));
    memset(s_npc_ext, 0, sizeof(s_npc_ext));   /* TD5RE full-name/collisions/air extensions */

    /* TD6 records start EMPTY (header -1) — never seeded with fake names. The
     * lazy loader (td6_records_ensure_loaded) fills genuine runs from disk. */
    memset(s_td6_records, 0, sizeof(s_td6_records));
    memset(s_td6_ext, 0, sizeof(s_td6_ext));
    for (i = 0; i < TD5_MAX_TD6_RECORD_LEVELS; i++)
        s_td6_records[i].header = -1;
    s_td6_records_loaded = 0;

    /* Default audio */
    s_sfx_volume = 80;
    s_music_volume = 80;

    /* All cars and race tracks unlocked by default.
     * Cars 0-36: all unlocked.
     * Tracks 0-19: unlocked (race tracks).
     * Tracks 20-25: locked (cup tracks, unlocked by winning cups).
     * td5_save_load_config() below will override with save file contents. */
    s_cup_tier = 0x00;
    s_max_unlocked_car = TD5_CONFIG_NUM_CARS; /* all 37 visible */
    s_all_cars_unlocked = 1;
    memset(s_car_locks, 0, sizeof(s_car_locks)); /* 0 = unlocked */
    for (i = 0; i < 20; i++)
        s_track_locks[i] = 1;  /* 1 = unlocked */
    for (i = 20; i < TD5_CONFIG_NUM_TRACKS; i++)
        s_track_locks[i] = 0;  /* 0 = locked (cup tracks) */

    /* Persistent settings/bindings/progress used to live in the binary
     * Config.td5. They now live in organized human-readable INI files.
     *   - If td5re_input.ini + td5re_progress.ini exist: read them.
     *   - Else if a legacy Config.td5 exists: import it once (XOR/CRC decode),
     *     write the new INIs, and rename the binary to Config.td5.migrated.
     *   - Else (fresh install): write the new INIs from the defaults above so
     *     the user immediately has editable files. */
    {
        int have_input    = td5_plat_file_exists(cfgini_input_path());
        int have_progress = td5_plat_file_exists(cfgini_progress_path());

        if (have_input && have_progress) {
            cfgini_read_input();
            cfgini_read_progress();
        } else if (td5_plat_file_exists(TD5_CONFIG_FILENAME)
                   && td5_save_load_config(NULL)) {
            /* Legacy binary decoded into the statics -> emit the new files. */
            cfgini_write_input();
            cfgini_write_progress();
            td5_plat_file_rename(TD5_CONFIG_FILENAME, TD5_CONFIG_MIGRATED);
            TD5_LOG_I(LOG_TAG,
                      "migrated Config.td5 -> %s + %s (renamed legacy to %s)",
                      TD5RE_INPUT_INI, TD5RE_PROGRESS_INI, TD5_CONFIG_MIGRATED);
        } else {
            /* Fresh install: seed the INI files from defaults. If only one of
             * the two existed, read the present one first so we don't lose it. */
            if (have_input)    cfgini_read_input();
            if (have_progress) cfgini_read_progress();
            cfgini_write_input();
            cfgini_write_progress();
            TD5_LOG_I(LOG_TAG, "seeded %s + %s from defaults",
                      TD5RE_INPUT_INI, TD5RE_PROGRESS_INI);
        }
    }

    /* Force-unlock all cars and race tracks regardless of saved contents.
     * Cup tracks (20-25) remain locked -- they require cup wins. */
    s_max_unlocked_car = TD5_CONFIG_NUM_CARS;
    s_all_cars_unlocked = 1;
    memset(s_car_locks, 0, sizeof(s_car_locks));
    for (i = 0; i < 20; i++)
        s_track_locks[i] = 1;  /* 1 = unlocked */

    /* Replace original default NPC names (Frank, Raymond…) with celebrities
     * for leaderboard slots that have no genuine player score yet.
     * If CelebrityNamesAPI=1, first try to fetch fresh names from
     * randomuser.me; fall back to the hardcoded list on failure. */
    {
        char (*api_names)[16] = NULL;
        int api_count = 0;

        if (g_td5.ini.celebrity_names_api) {
            api_names = (char (*)[16])malloc(130 * 16);
            if (api_names) {
                memset(api_names, 0, 130 * 16);
                api_count = npc_fetch_api_names(api_names, 130);
                if (api_count == 0) {
                    TD5_LOG_W(LOG_TAG,
                              "celebrity API unavailable, using hardcoded list");
                    free(api_names);
                    api_names = NULL;
                }
            }
        }

        npc_apply_celebrity_names(api_names, api_count);

        if (api_names) free(api_names);
    }

    /* Flush progress so the file reflects celebrity names immediately (not
     * just the next time the player saves).  Cheap — one sequential write. */
    cfgini_write_progress();

    return 1;
}

void td5_save_shutdown(void)
{
    /* Nothing to release. */
}

/* ========================================================================
 * XOR Encryption (self-inverse)
 *
 * Original at multiple inline sites. Key index wraps via strlen() modulo.
 * The 0x80 flip ensures zero bytes don't directly reveal the key.
 * ======================================================================== */

void td5_save_xor_encrypt(uint8_t *data, size_t size, const char *key)
{
    size_t key_len = strlen(key);
    size_t ki = 0;
    for (size_t i = 0; i < size; i++) {
        data[i] = data[i] ^ (uint8_t)key[ki] ^ 0x80;
        ki++;
        if (ki == key_len) {
            ki = 0;
        }
    }
}

void td5_save_xor_decrypt(uint8_t *data, size_t size, const char *key)
{
    /* Self-inverse: encrypt == decrypt. */
    td5_save_xor_encrypt(data, size, key);
}

/* ========================================================================
 * CRC-32 (standard ISO 3309 / ITU-T V.42)
 *
 * Polynomial 0xEDB88320 (reflected). Init = 0xFFFFFFFF, final XOR = 0xFFFFFFFF.
 * Original lookup table at VA 0x475160.
 * ======================================================================== */

uint32_t td5_save_crc32(const uint8_t *data, size_t size)
{
    return td5_crc32(data, size);
}

/* ========================================================================
 * Helper: read/write a little-endian uint32 from a byte buffer
 * ======================================================================== */

static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/* ========================================================================
 * WritePackedConfigTd5  (original VA 0x40F8D0)
 *
 * Serializes all persistent settings into a 5351-byte buffer, computes
 * CRC-32, XOR-encrypts with "Outta Mah Face !! ", writes to Config.td5.
 * Returns 1 on success, 0 on failure.
 * ======================================================================== */

/* Retained for reference / potential debug export. The live write path now
 * targets the organized INI files (see td5_save_write_config), so this is no
 * longer reached at runtime -- hence the unused attribute to keep -Wextra quiet. */
static void config_serialize_to_buffer(void) __attribute__((unused));
static void config_serialize_to_buffer(void)
{
    TD5_ConfigBuffer *buf = (TD5_ConfigBuffer *)s_config_buf;

    /* Game options: 7 dwords at offset 0x04. */
    memcpy(buf->game_options, &s_game_options, sizeof(buf->game_options));

    /* Controller device indices (legacy blob only stores p1/p2). */
    buf->p1_device_index = (uint8_t)s_player_device_index[0];
    buf->p2_device_index = (uint8_t)s_player_device_index[1];

    /* Force feedback config (4 dwords in non-sequential order matching original). */
    buf->ff_config_a = s_ff_config[0];  /* 0x464054 */
    buf->ff_config_b = s_ff_config[1];  /* 0x46405C */
    buf->ff_config_c = s_ff_config[2];  /* 0x464058 */
    buf->ff_config_d = s_ff_config[3];  /* 0x464060 */

    /* Controller binding tables. */
    memcpy(buf->controller_bindings, s_controller_bindings, sizeof(buf->controller_bindings));

    /* Device descriptors (from active slots on write). */
    memcpy(buf->p1_device_desc, s_p1_device_desc, sizeof(buf->p1_device_desc));
    memcpy(buf->p2_device_desc, s_p2_device_desc, sizeof(buf->p2_device_desc));

    /* Audio settings. */
    buf->sound_mode   = (uint8_t)s_sound_mode;
    buf->sfx_volume   = (uint8_t)s_sfx_volume;
    buf->music_volume = (uint8_t)s_music_volume;

    /* Display settings. */
    buf->display_mode_ordinal = s_display_mode;
    buf->fog_enabled          = s_fog_enabled;
    buf->speed_units          = s_speed_units;
    buf->camera_damping       = s_camera_damping;

    /* Custom binding maps. */
    memcpy(buf->p1_custom_bindings, s_p1_custom_bindings, sizeof(buf->p1_custom_bindings));
    memcpy(buf->p2_custom_bindings, s_p2_custom_bindings, sizeof(buf->p2_custom_bindings));

    /* Misc single-byte settings. */
    buf->split_screen_mode = (uint8_t)s_split_screen_mode;
    buf->catchup_assist    = (uint8_t)s_catchup_assist;
    buf->camera_byte_a     = s_camera_byte_a;
    buf->camera_byte_b     = s_camera_byte_b;

    /* NPC racer group table (high scores). */
    memcpy(buf->npc_group_table, s_npc_group_table, sizeof(buf->npc_group_table));

    /* Reserved zero byte + music track. */
    buf->reserved_zero = 0;
    buf->music_track   = (uint8_t)s_music_track;

    /* Progression state. */
    buf->cup_tier_state     = (uint8_t)(s_cup_tier & 0x07);
    buf->max_unlocked_car   = (uint8_t)s_max_unlocked_car;
    buf->all_cars_unlocked  = s_all_cars_unlocked;

    /* Lock tables. */
    memcpy(buf->track_locks, s_track_locks, sizeof(buf->track_locks));
    memcpy(buf->car_locks,   s_car_locks,   sizeof(buf->car_locks));

    /* Cheat flags: only bit 0 is persisted per entry (original masks with & 1). */
    for (int i = 0; i < TD5_CONFIG_NUM_CHEATS; i++) {
        buf->cheat_flags[i] = s_cheat_flags[i] & 0x01;
    }
}

int td5_save_write_config(const char *path)
{
    /* Config.td5 retired (2026-06-03): persist the live bindings + high-score /
     * unlock / cheat state to the organized human-readable INI files instead.
     * The `path` argument (historically a Config.td5 filename) is ignored;
     * every existing caller just means "persist the current save state". */
    (void)path;
    int ok_in = cfgini_write_input();
    int ok_pr = cfgini_write_progress();
    TD5_LOG_I(LOG_TAG, "Write config -> %s(%s) + %s(%s)",
              TD5RE_INPUT_INI,    ok_in ? "ok" : "fail",
              TD5RE_PROGRESS_INI, ok_pr ? "ok" : "fail");
    return (ok_in && ok_pr) ? 1 : 0;
}

/* ========================================================================
 * LoadPackedConfigTd5  (original VA 0x40FB60)
 *
 * Reads Config.td5, XOR-decrypts, validates CRC-32, restores all globals.
 * Returns 1 on success, 0 on failure (missing file or bad CRC).
 *
 * Load asymmetry: device descriptors are written from active slots
 * (0x465660/0x465680) but restored to backup slots (0x4656A0/0x4656C0).
 * ======================================================================== */

static void config_deserialize_from_buffer(void)
{
    const TD5_ConfigBuffer *buf = (const TD5_ConfigBuffer *)s_config_buf;

    /* Game options. */
    memcpy(&s_game_options, buf->game_options, sizeof(s_game_options));

    /* Controller device indices (legacy blob supplies p1/p2; mirror into the
     * generic per-player array, players 3-9 default to keyboard until the
     * td5re_input.ini overlay or the menu sets them). */
    s_p1_device_index = buf->p1_device_index;
    s_p2_device_index = buf->p2_device_index;
    s_player_device_index[0] = s_p1_device_index;
    s_player_device_index[1] = s_p2_device_index;

    /* Force feedback config. */
    s_ff_config[0] = buf->ff_config_a;
    s_ff_config[1] = buf->ff_config_b;
    s_ff_config[2] = buf->ff_config_c;
    s_ff_config[3] = buf->ff_config_d;

    /* Controller binding tables. */
    memcpy(s_controller_bindings, buf->controller_bindings, sizeof(s_controller_bindings));

    /* Device descriptors: restore to BACKUP slots (load asymmetry). */
    memcpy(s_p1_device_desc_backup, buf->p1_device_desc, sizeof(s_p1_device_desc_backup));
    memcpy(s_p2_device_desc_backup, buf->p2_device_desc, sizeof(s_p2_device_desc_backup));

    /* Audio settings. */
    s_sound_mode   = buf->sound_mode;
    s_sfx_volume   = buf->sfx_volume;
    s_music_volume = buf->music_volume;

    /* Display settings. */
    s_display_mode   = buf->display_mode_ordinal;
    s_fog_enabled    = buf->fog_enabled;
    s_speed_units    = buf->speed_units;
    s_camera_damping = buf->camera_damping;

    /* Custom binding maps. */
    memcpy(s_p1_custom_bindings, buf->p1_custom_bindings, sizeof(s_p1_custom_bindings));
    memcpy(s_p2_custom_bindings, buf->p2_custom_bindings, sizeof(s_p2_custom_bindings));

    /* Push the loaded keyboard scancodes (first 10 bytes of each custom-binding
     * buffer, canonical action order) down to the input layer so saved rebinds
     * take effect in-race. A 0 byte is ignored by the setter, so an old save
     * with empty bindings keeps the built-in defaults. */
    td5_plat_input_set_keyboard_bindings(0, (const uint8_t *)s_p1_custom_bindings, 10);
    td5_plat_input_set_keyboard_bindings(1, (const uint8_t *)s_p2_custom_bindings, 10);

    /* Misc single-byte settings. */
    s_split_screen_mode = buf->split_screen_mode;
    s_catchup_assist    = buf->catchup_assist;
    s_camera_byte_a     = buf->camera_byte_a;
    s_camera_byte_b     = buf->camera_byte_b;
    s_music_track       = buf->music_track;

    /* NPC racer group table (high scores). */
    memcpy(s_npc_group_table, buf->npc_group_table, sizeof(s_npc_group_table));

    /* Progression state. */
    s_cup_tier          = buf->cup_tier_state;
    s_max_unlocked_car  = buf->max_unlocked_car;
    s_all_cars_unlocked = buf->all_cars_unlocked;

    /* Lock tables. */
    memcpy(s_track_locks, buf->track_locks, sizeof(s_track_locks));
    memcpy(s_car_locks,   buf->car_locks,   sizeof(s_car_locks));

    /* Cheat flags (26 bytes). */
    memcpy(s_cheat_flags, buf->cheat_flags, sizeof(s_cheat_flags));
}

int td5_save_load_config(const char *path)
{
    const char *filepath = path ? path : TD5_CONFIG_FILENAME;

    /* Step 1: Open and read the file. */
    TD5_File *f = td5_plat_file_open(filepath, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Load config failed: path=%s reason=open", filepath);
        return 0;
    }

    uint8_t read_buf[TD5_CONFIG_READ_BUF_SIZE];
    size_t bytes_read = td5_plat_file_read(f, read_buf, TD5_CONFIG_READ_BUF_SIZE);
    td5_plat_file_close(f);

    if (bytes_read == 0) {
        TD5_LOG_W(LOG_TAG, "Load config failed: path=%s reason=empty", filepath);
        return 0;
    }

    /* Step 2: XOR-decrypt. */
    td5_save_xor_decrypt(read_buf, bytes_read, TD5_CONFIG_XOR_KEY);

    /* Step 3: Save stored CRC, replace with placeholder, recompute. */
    uint32_t stored_crc = read_le32(read_buf);
    write_le32(read_buf, TD5_CRC_PLACEHOLDER);

    uint32_t computed_crc = td5_save_crc32(read_buf, bytes_read);

    if (computed_crc != stored_crc) {
        TD5_LOG_W(LOG_TAG, "Load config failed: path=%s crc=0x%08X expected=0x%08X size=%u",
                  filepath, (unsigned int)computed_crc, (unsigned int)stored_crc,
                  (unsigned int)bytes_read);
        return 0;
    }

    /* Step 4: Copy into our working buffer and deserialize. */
    memcpy(s_config_buf, read_buf, TD5_CONFIG_FILE_SIZE < bytes_read ? TD5_CONFIG_FILE_SIZE : bytes_read);
    config_deserialize_from_buffer();
    TD5_LOG_I(LOG_TAG, "Load config ok: path=%s crc=0x%08X size=%u",
              filepath, (unsigned int)stored_crc, (unsigned int)bytes_read);

    return 1;
}

/* ========================================================================
 * SerializeRaceStatusSnapshot  (original VA 0x411120)
 *
 * Captures the entire cup state into the contiguous 12966-byte snapshot
 * buffer. Computes CRC-32 and stores it at offset +0x0C.
 *
 * [ARCH-DIVERGENCE: port-only TD5_CUPDATA_OVERLAY trailer; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00411120: orig writes exactly 0x32A6 bytes (header
 *   bytes 0x00..0x0B, race-schedule at 0x10, race-results at 0x88, actor
 *   block at 0x100 (0xC5C dwords), slot-state at 0x3270 (6 dwords), tail
 *   misc-state at 0x3288..0x32A5), CRC32 over all 0x32A6 bytes, stores
 *   at +0x0C, sets g_snapshotPayloadSize = 0x32A6. The port mirrors every
 *   offset byte-for-byte within the first 0x32A6 bytes (including the
 *   asymmetric 0x328C/0x3290/0x3294/0x3296/0x3297/0x329B/0x329F/0x32A3/
 *   0x32A5 packing) then appends a 32-byte port-only "TD5RECUP" overlay
 *   carrying 6 stable car_index entries, extending the file to
 *   TD5_CUPDATA_EXT_FILE_SIZE (12998 B). CRC32 covers the extended range
 *   (CRC field set to TD5_CRC_PLACEHOLDER first to mirror orig's
 *   placeholder dance). Legacy files (size == 0x32A6) still load via
 *   the symmetric reader path. */

/* Retained for reference; the live cup write path now targets td5re_cup.ini
 * (see td5_save_write_cup_data -> cfgini_write_cup), so this binary serializer
 * is no longer reached at runtime. */
static void cup_serialize_to_buffer(void) __attribute__((unused));
static void cup_serialize_to_buffer(void)
{
    uint8_t *buf = s_cup_buf;

    /* Header bytes: individual flags packed into bytes 0x00..0x0B. */
    buf[0x00] = (uint8_t)s_selected_game_type;
    buf[0x01] = (uint8_t)s_race_within_series;
    buf[0x02] = (uint8_t)s_npc_group_index;
    buf[0x03] = (uint8_t)s_track_opponent_state;
    buf[0x04] = (uint8_t)s_race_rule_variant;
    buf[0x05] = (uint8_t)s_time_trial_enabled;
    buf[0x06] = (uint8_t)s_wanted_enabled;
    buf[0x07] = (uint8_t)s_difficulty_tier;
    buf[0x08] = (uint8_t)s_checkpoint_mode;
    buf[0x09] = (uint8_t)s_traffic_enabled;
    buf[0x0A] = (uint8_t)s_special_encounter;
    buf[0x0B] = (uint8_t)s_circuit_lap_count;

    /* Race schedule (120 bytes at offset 0x10). */
    memcpy(buf + 0x10, s_race_schedule, 0x1E * 4);

    /* Race results (120 bytes at offset 0x88). */
    memcpy(buf + 0x88, s_race_results, 0x1E * 4);

    /* Full actor state (12656 bytes at offset 0x100). */
    memcpy(buf + 0x0100, s_actor_table, 0xC5C * 4);

    /* Slot state (24 bytes at offset 0x3270). */
    memcpy(buf + 0x3270, s_slot_state, 6 * 4);

    /* Masters mode state and cup sub-state fields in the tail region.
     * The original code stores these at specific non-aligned offsets
     * relative to the snapshot buffer base (0x490BAC). We replicate
     * the exact byte offsets.
     *
     * Original addresses -> buffer offsets:
     *   0x493E34 - 0x490BAC = 0x3288  (masters_schedule_base = 0x48F30C)
     *   0x493E3C - 0x490BAC = 0x3290  (p1_cup_schedule_index = 0x48F314)
     *   0x493E38 - 0x490BAC = 0x328C  (p2_cup_schedule_index = 0x48F310)  -- NOTE: 0x328C not 0x3294
     *   0x493E42 - 0x490BAC = 0x3296  (p1_selected_cup_id  = 0x48F31A)
     *   0x493E40 - 0x490BAC = 0x3294  (p1_cup_completion_bitmask  = 0x48F318)  -- NOTE: 0x3294 not 0x3297
     *   0x493E47 - 0x490BAC = 0x329B  (p1_masters_unlock_bitmask = 0x48F328)
     *   0x493E43 - 0x490BAC = 0x3297  (masters_encounter_flags = 0x48F324)
     *   0x493E4B - 0x490BAC = 0x329F  (p2_masters_unlock_bitmask = 0x48F32C)
     *   0x493E4F - 0x490BAC = 0x32A3  (p2_cup_completion_bitmask = 0x48F330)
     *   0x493E51 - 0x490BAC = 0x32A5  (p2_cup_lock_flag = 0x48F332)
     */
    write_le32(buf + 0x3288, s_masters_schedule_base);
    write_le32(buf + 0x328C, s_p2_cup_schedule_index);
    write_le32(buf + 0x3290, s_p1_cup_schedule_index);
    write_le16(buf + 0x3294, s_p1_cup_completion_bitmask);
    buf[0x3296] = s_p1_selected_cup_id;
    write_le32(buf + 0x3297, s_masters_encounter_flags);
    write_le32(buf + 0x329B, s_p1_masters_unlock_bitmask);
    write_le32(buf + 0x329F, s_p2_masters_unlock_bitmask); /* VERIFIED: 0x48F32C, addr calc 0x493E4B-0x490BAC=0x329F */
    write_le16(buf + 0x32A3, s_p2_cup_completion_bitmask);
    buf[0x32A5] = s_p2_cup_lock_flag;

    /* Compute CRC-32 over the entire buffer with placeholder at offset +0x0C. */
    /* The original sets bytes [0x0C..0x0F] to placeholder before CRC. */
    /* Actually, the original computes CRC over the full buffer as-is and then
     * stores the result at offset +0x0C. Looking at the decompiled code:
     * the CRC loop runs over all 0x32A6 bytes, then DAT_00490BB8 = ~crc.
     * The CRC field bytes are whatever was in the buffer during computation.
     * Since this is freshly serialized, the CRC field has leftover/zero data.
     * The exact original behavior: compute CRC over the full buffer (CRC field
     * contains stale data), then overwrite with result. The restore function
     * recomputes over the full buffer including the CRC field and checks
     * that the stored CRC matches the computed CRC. This works because
     * on restore, the CRC field contains the CRC itself, so the recomputation
     * must produce the same value. Wait -- that can't work either...
     *
     * Re-examining the Ghidra output for RestoreRaceStatusSnapshot:
     *   uVar2 = 0xFFFFFFFF;
     *   do { uVar2 = ... CRC over full buffer ... } while (uVar3 < DAT_00494bbc);
     *   if (DAT_00490bb8 == ~uVar2) { ... }
     * This checks: stored_crc_at_0x0C == CRC32(full_buffer).
     * But the full_buffer includes the CRC bytes at 0x0C!
     *
     * For SerializeRaceStatusSnapshot, the CRC loop also runs over all bytes.
     * Before the CRC loop, the buffer already has all fields written.
     * The CRC field at 0x0C was not explicitly set to a placeholder in the
     * decompiled code. It just computes CRC over whatever is there, then
     * stores the result. This means the CRC is computed with the CRC field
     * containing whatever value was previously in the buffer (likely 0 for
     * freshly zeroed memory, or stale from a previous serialization).
     *
     * BUT: on restore, the CRC field now contains the stored CRC, and the
     * recomputation includes those bytes. For this to match, we need:
     *   CRC32(buf_with_crc_at_0C) == stored_crc
     * where stored_crc = CRC32(buf_with_old_crc_at_0C).
     * These can only match if the CRC field had the SAME value during both
     * computations. Since the write stores the computed CRC into the field,
     * the restore computation includes the stored CRC bytes. This means
     * CRC32(buf_with_stored_crc) must equal stored_crc. This is only true
     * if there's a CRC-32 fixed point property being exploited, OR...
     *
     * Actually, let me re-read the decompiled code more carefully.
     * The serializer stores the CRC AFTER computing it. So:
     *   1. All fields written (CRC field = don't care)
     *   2. CRC = CRC32(all 0x32A6 bytes including whatever is at 0x0C)
     *   3. Store CRC at offset 0x0C
     * The restorer:
     *   1. CRC field now = stored CRC from step 3
     *   2. Recompute CRC = CRC32(all bytes including stored CRC at 0x0C)
     *   3. Compare stored CRC == recomputed CRC
     *
     * These won't match unless the CRC at 0x0C was the same during both
     * computations. They're NOT the same (first time it's stale/zero,
     * second time it's the CRC itself).
     *
     * The ONLY way this works is if the original code DOES set a placeholder.
     * Looking at the decompiled serializer again... I don't see an explicit
     * placeholder set. But looking at the analysis doc, it says:
     *   "Sets the first 4 bytes at 0x490BB8 to placeholder 0x10, 0x00, 0x00, 0x00"
     * And 0x490BB8 = 0x490BAC + 0x0C = offset 0x0C in the buffer.
     *
     * The Ghidra decompiler may have missed this because the placeholder
     * store gets folded into the CRC computation (the CRC init 0xFFFFFFFF
     * overlaps with the buffer write). Let me trust the analysis doc.
     */
    /* TD5RE divergent overlay (port-only). Persist per-slot car indices
     * so the loader can re-resolve actor+0x1B0/+0x1B8/+0x1BC via the
     * asset registry instead of trusting raw pointers from the .data
     * segment of an earlier build. See td5_types.h overlay block.
     *
     * Slot mapping mirrors td5_frontend.c:
     *   slot 0       -> g_td5.car_index (player)
     *   slot 1..5    -> g_td5.ai_car_indices[1..5]
     * (g_td5.ai_car_indices[0] is unused.) */
    memcpy(buf + TD5_CUPDATA_OVERLAY_OFFSET,
           TD5_CUPDATA_OVERLAY_MAGIC, TD5_CUPDATA_OVERLAY_MAGIC_LEN);
    {
        uint8_t *ids = buf + TD5_CUPDATA_OVERLAY_OFFSET +
                       TD5_CUPDATA_OVERLAY_MAGIC_LEN;
        write_le32(ids + 0 * 4, (uint32_t)g_td5.car_index);
        write_le32(ids + 1 * 4, (uint32_t)g_td5.ai_car_indices[1]);
        write_le32(ids + 2 * 4, (uint32_t)g_td5.ai_car_indices[2]);
        write_le32(ids + 3 * 4, (uint32_t)g_td5.ai_car_indices[3]);
        write_le32(ids + 4 * 4, (uint32_t)g_td5.ai_car_indices[4]);
        write_le32(ids + 5 * 4, (uint32_t)g_td5.ai_car_indices[5]);
    }

    /* CRC-32 over the full extended buffer (placeholder dance preserves
     * symmetry with the original — see RestoreRaceStatusSnapshot). */
    write_le32(buf + 0x0C, TD5_CRC_PLACEHOLDER);
    uint32_t crc = td5_save_crc32(buf, TD5_CUPDATA_EXT_FILE_SIZE);
    write_le32(buf + 0x0C, crc);

    /* Store snapshot size — extended format. */
    s_cup_buf_size = TD5_CUPDATA_EXT_FILE_SIZE;

    TD5_LOG_I(LOG_TAG, "cup serialize: ext format, ids=[%d,%d,%d,%d,%d,%d]",
              g_td5.car_index,
              g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
              g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
              g_td5.ai_car_indices[5]);
}

/* ========================================================================
 * RestoreRaceStatusSnapshot  (original VA 0x4112C0)
 *
 * Validates CRC-32 embedded at offset +0x0C, then restores all cup globals
 * from the snapshot buffer. Returns 1 on success, 0 on failure.
 *
 * [ARCH-DIVERGENCE: port-only TD5_CUPDATA_OVERLAY trailer; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x004112C0: orig reads at fixed 0x32A6 size, validates
 *   CRC32 stored at +0x0C against recomputation, then unpacks the same
 *   byte offsets the serializer writes. The port mirrors every offset
 *   byte-for-byte (including the special game_type==0xFF -> -1 remap
 *   and the asymmetric 0x328C/0x3290/0x3294/0x3296/0x3297/0x329B/0x329F/
 *   0x32A3/0x32A5 unpacking) plus a port-only path that recognizes the
 *   "TD5RECUP" magic at offset 0x32A6 and decodes 6 car_index entries
 *   into s_overlay_car_indices for the source-port's actor pointer
 *   re-resolution pass. Defensive: any restored racer actor (slots 0..5)
 *   has its pointer-shaped fields at +0x1B0/+0x1B8/+0x1BC scrubbed so
 *   stale dangling pointers cannot be dereferenced before
 *   LoadRaceVehicleAssets/InitializeRaceActor re-fill them. Legacy
 *   (non-overlay) files validate identically to orig. */

static int cup_deserialize_from_buffer(void)
{
    uint8_t *buf = s_cup_buf;

    if (s_cup_buf_size == 0) {
        return 0;
    }

    /* Save stored CRC, set placeholder, recompute, compare. */
    uint32_t stored_crc = read_le32(buf + 0x0C);
    write_le32(buf + 0x0C, TD5_CRC_PLACEHOLDER);

    uint32_t computed_crc = td5_save_crc32(buf, s_cup_buf_size);

    if (stored_crc != computed_crc) {
        /* Restore the CRC field before returning. */
        write_le32(buf + 0x0C, stored_crc);
        return 0;
    }

    /* Restore the CRC field. */
    write_le32(buf + 0x0C, stored_crc);

    /* Restore header flag bytes. */
    uint32_t game_type_raw = buf[0x00];
    s_selected_game_type  = game_type_raw;
    s_race_within_series  = buf[0x01];
    s_npc_group_index     = buf[0x02];
    s_track_opponent_state = buf[0x03];
    s_race_rule_variant   = buf[0x04];
    s_time_trial_enabled  = buf[0x05];
    s_wanted_enabled      = buf[0x06];
    s_difficulty_tier     = buf[0x07];
    s_checkpoint_mode     = buf[0x08];
    s_traffic_enabled     = buf[0x09];
    s_special_encounter   = buf[0x0A];
    s_circuit_lap_count   = buf[0x0B];

    /* Race schedule. */
    memcpy(s_race_schedule, buf + 0x10, 0x1E * 4);

    /* Race results. */
    memcpy(s_race_results, buf + 0x88, 0x1E * 4);

    /* Full actor state. */
    memcpy(s_actor_table, buf + 0x0100, 0xC5C * 4);

    /* Slot state. */
    memcpy(s_slot_state, buf + 0x3270, 6 * 4);

    /* Masters/cup sub-state fields (tail region). */
    s_masters_schedule_base   = read_le32(buf + 0x3288);
    s_p2_cup_schedule_index         = read_le32(buf + 0x328C);
    s_p1_cup_schedule_index         = read_le32(buf + 0x3290);
    s_p1_cup_completion_bitmask          = read_le16(buf + 0x3294);
    s_p1_selected_cup_id          = buf[0x3296];
    s_masters_encounter_flags = read_le32(buf + 0x3297);
    s_p1_masters_unlock_bitmask         = read_le32(buf + 0x329B);
    s_p2_masters_unlock_bitmask         = read_le32(buf + 0x329F); /* VERIFIED: 0x48F32C */
    s_p2_cup_completion_bitmask          = read_le16(buf + 0x32A3);
    s_p2_cup_lock_flag          = buf[0x32A5];

    /* Cross-references within the schedule region.
     * These are at fixed offsets from the schedule base (buf + 0x10):
     *   0x490BD4 - 0x490BAC = 0x28  -> buf[0x10 + 0x18] = buf[0x28]
     *   0x490BEC - 0x490BAC = 0x40  -> buf[0x40]
     *   0x490C04 - 0x490BAC = 0x58  -> buf[0x58]
     *   0x490C1C - 0x490BAC = 0x70  -> buf[0x70]
     */
    s_cup_progress_marker = read_le32(buf + 0x28);
    s_cup_cross_ref_1     = read_le32(buf + 0x40);
    s_cup_cross_ref_2     = read_le32(buf + 0x58);
    s_cup_cross_ref_3     = read_le32(buf + 0x70);

    /* Special case: game type 0xFF remaps to -1 (no active cup). */
    if (game_type_raw == 0xFF) {
        s_selected_game_type = 0xFFFFFFFF; /* -1 as unsigned */
    }

    /* TD5RE divergent overlay parse (port-only). When the file is in
     * extended format, the 32-byte overlay at +0x32A6 carries 6 stable
     * car_index values that the loader uses to re-resolve actor pointer
     * fields via the asset registry on race re-init. Defensive: also
     * NULL the three pointer slots inside each restored racer-actor
     * (offsets +0x1B0/+0x1B8/+0x1BC) so any stale dangling pointer in
     * the saved blob can't be dereferenced before LoadRaceVehicleAssets
     * + InitializeRaceActor (FUN_0042F140 in original) re-fill them. */
    s_overlay_present = 0;
    for (int i = 0; i < TD5_CUPDATA_OVERLAY_NUM_SLOTS; i++) {
        s_overlay_car_indices[i] = -1;
    }
    if (s_cup_buf_size >= TD5_CUPDATA_EXT_FILE_SIZE &&
        memcmp(buf + TD5_CUPDATA_OVERLAY_OFFSET,
               TD5_CUPDATA_OVERLAY_MAGIC,
               TD5_CUPDATA_OVERLAY_MAGIC_LEN) == 0) {
        const uint8_t *ids = buf + TD5_CUPDATA_OVERLAY_OFFSET +
                             TD5_CUPDATA_OVERLAY_MAGIC_LEN;
        for (int i = 0; i < TD5_CUPDATA_OVERLAY_NUM_SLOTS; i++) {
            s_overlay_car_indices[i] = (int)read_le32(ids + i * 4);
        }
        s_overlay_present = 1;
        /* Defensive pointer-slot scrub for racer slots 0..5. */
        for (int slot = 0; slot < TD5_CUPDATA_OVERLAY_NUM_SLOTS; slot++) {
            uint8_t *a = (uint8_t *)s_actor_table + (size_t)slot * 0x388;
            write_le32(a + 0x1B0, 0);
            write_le32(a + 0x1B8, 0);
            write_le32(a + 0x1BC, 0);
        }
        TD5_LOG_I(LOG_TAG,
                  "cup deserialize: overlay applied ids=[%d,%d,%d,%d,%d,%d]",
                  s_overlay_car_indices[0], s_overlay_car_indices[1],
                  s_overlay_car_indices[2], s_overlay_car_indices[3],
                  s_overlay_car_indices[4], s_overlay_car_indices[5]);
    } else {
        TD5_LOG_I(LOG_TAG,
                  "cup deserialize: legacy format size=%u (no overlay)",
                  (unsigned int)s_cup_buf_size);
    }

    return 1;
}

/* ========================================================================
 * WriteCupData  (original VA 0x4114F0)
 *
 * XOR-encrypts a copy of the snapshot buffer and writes to CupData.td5.
 * The original allocates the encrypted copy on the stack (huge alloca).
 * Returns 1 on success, 0 on failure.
 * ======================================================================== */

int td5_save_write_cup_data(const char *path)
{
    /* CupData.td5 retired (2026-06-03): write the human-readable cup resume
     * state to td5re_cup.ini (no per-actor physics snapshot). The caller has
     * already run td5_save_sync_cup_from_game() to populate the cup statics.
     * A non-NULL path is honoured as an alternate INI target (used by the
     * dev-only roundtrip self-test). */
    int ok = cfgini_write_cup(path ? path : cfgini_cup_path());
    TD5_LOG_I(LOG_TAG, "Write cup data %s -> %s",
              ok ? "ok" : "failed", path ? path : TD5RE_CUP_INI);
    return ok;
}

/* ========================================================================
 * LoadContinueCupData  (original VA 0x411590)
 *
 * Legacy binary reader, retained ONLY for one-time migration of an existing
 * CupData.td5: read + XOR-decrypt into the snapshot buffer + CRC-validate +
 * restore the cup statics (does NOT touch g_td5 / the actor table -- that is
 * td5_save_sync_cup_to_game's job).
 * ======================================================================== */

static int cup_load_binary_file(const char *path)
{
    const char *filepath = path ? path : TD5_CUPDATA_FILENAME;
    int ok;

    TD5_File *f = td5_plat_file_open(filepath, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Load cup data failed: path=%s reason=open", filepath);
        return 0;
    }

    uint8_t read_buf[TD5_CUPDATA_READ_BUF_SIZE];
    size_t bytes_read = td5_plat_file_read(f, read_buf, TD5_CUPDATA_READ_BUF_SIZE);
    td5_plat_file_close(f);

    if (bytes_read == 0) {
        TD5_LOG_W(LOG_TAG, "Load cup data failed: path=%s reason=empty", filepath);
        return 0;
    }

    for (size_t i = 0; i < bytes_read; i++) {
        s_cup_buf[i] = read_buf[i];
    }
    td5_save_xor_decrypt(s_cup_buf, bytes_read, TD5_CUPDATA_XOR_KEY);
    s_cup_buf_size = (uint32_t)bytes_read;

    ok = cup_deserialize_from_buffer();
    TD5_LOG_I(LOG_TAG, "Load legacy cup data %s: path=%s size=%u",
              ok ? "ok" : "failed", filepath, (unsigned int)bytes_read);
    return ok;
}

int td5_save_load_cup_data(const char *path)
{
    /* New path: read td5re_cup.ini into the cup statics. If it's absent but a
     * legacy CupData.td5 exists, import it once (binary decode -> cup INI ->
     * rename binary to .migrated), then read the new INI. A non-NULL path is
     * treated as an explicit INI target (dev-only roundtrip self-test). */
    const char *cup_ini = path ? path : cfgini_cup_path();

    if (!path && !td5_plat_file_exists(cup_ini)
        && td5_plat_file_exists(TD5_CUPDATA_FILENAME)) {
        if (cup_load_binary_file(NULL)) {
            cfgini_write_cup(cup_ini);
            td5_plat_file_rename(TD5_CUPDATA_FILENAME, TD5_CUPDATA_MIGRATED);
            TD5_LOG_I(LOG_TAG, "migrated CupData.td5 -> %s (renamed legacy to %s)",
                      TD5RE_CUP_INI, TD5_CUPDATA_MIGRATED);
        }
    }

    int ok = cfgini_read_cup(cup_ini);
    TD5_LOG_I(LOG_TAG, "Load cup data %s -> %s",
              ok ? "ok" : "failed", cup_ini);
    return ok;
}

/* ========================================================================
 * ValidateCupDataChecksum  (original VA 0x411630)
 *
 * Re-reads CupData.td5, decrypts, computes CRC-32 over the full buffer,
 * and compares against the expected CRC passed in. Used to check whether
 * a valid cup save exists without restoring it (enables/disables the
 * "Continue Cup" button on the race type menu).
 *
 * Original signature: bool __cdecl(unused, unused, uint32_t expected_crc)
 * The first two parameters are unused in the original binary.
 *
 * [CONFIRMED @ 0x00411630 ValidateCupDataChecksum; L5 sweep 2026-05-21]
 *   Byte-faithful: open via filename -> fread up to 0x4000 bytes ->
 *   XOR-decrypt with TD5_CUPDATA_XOR_KEY (orig's `g_cupDataXorKey[iVar6] ^
 *   buf[uVar7] ^ 0x80` with key-index wraparound on NUL is the same as
 *   td5_save_xor_decrypt) -> CRC32 over the full bytes_read window ->
 *   compare to expected. Port reads expected_crc from an explicit
 *   parameter; orig reads it from an above-buffer stack slot (cdecl
 *   caller pushes it). Same algorithm, same buffer cap, same key. */

int td5_save_validate_cup_checksum(const char *path, uint32_t expected_crc)
{
    const char *filepath = path ? path : TD5_CUPDATA_FILENAME;

    /* Step 1: Open and read. */
    TD5_File *f = td5_plat_file_open(filepath, "rb");
    if (!f) {
        return 0;
    }

    uint8_t read_buf[TD5_CUPDATA_READ_BUF_SIZE];
    size_t bytes_read = td5_plat_file_read(f, read_buf, TD5_CUPDATA_READ_BUF_SIZE);
    td5_plat_file_close(f);

    if (bytes_read == 0) {
        return 0;
    }

    /* Step 2: XOR-decrypt in-place. */
    td5_save_xor_decrypt(read_buf, bytes_read, TD5_CUPDATA_XOR_KEY);

    /* Step 3: Compute CRC-32 over the full decrypted buffer. */
    uint32_t computed_crc = td5_save_crc32(read_buf, bytes_read);

    /* Step 4: Compare against expected. */
    return (expected_crc == computed_crc) ? 1 : 0;
}

/* ========================================================================
 * Cup state sync (source-port bridge)
 *
 * In the original binary these globals lived at fixed addresses shared
 * by both the frontend and save code.  In the source port each module
 * keeps private statics, so we explicitly copy before write / after load.
 * ======================================================================== */

void td5_save_sync_cup_from_game(int race_within_series)
{
    /* Header fields from g_td5 */
    s_selected_game_type  = (uint32_t)g_td5.game_type;
    s_race_within_series  = (uint32_t)race_within_series;
    s_race_rule_variant   = (uint32_t)g_td5.race_rule_variant;
    s_time_trial_enabled  = (uint32_t)g_td5.time_trial_enabled;
    s_wanted_enabled      = (uint32_t)g_td5.wanted_mode_enabled;
    s_difficulty_tier     = (uint32_t)g_td5.difficulty;
    s_checkpoint_mode     = (uint32_t)g_td5.checkpoint_timers_enabled;
    s_traffic_enabled     = (uint32_t)g_td5.traffic_enabled;
    s_special_encounter   = (uint32_t)g_td5.special_encounter_enabled;
    s_circuit_lap_count   = (uint32_t)g_td5.circuit_lap_count;

    /* npc_group_index and track_opponent_state: not tracked by frontend
     * in the source port yet -- leave as-is (previously serialized value
     * or 0 on first save). */

    /* Actor snapshot: copy raw bytes from game module actor table. */
    {
        int total = td5_game_get_total_actor_count();
        int i;
        memset(s_actor_table, 0, sizeof(s_actor_table));
        for (i = 0; i < total && i < 14; i++) {
            TD5_Actor *a = td5_game_get_actor(i);
            if (a) {
                memcpy((uint8_t *)s_actor_table + (size_t)i * 0x388,
                       a, 0x388);
            }
        }
    }

    TD5_LOG_I(LOG_TAG, "sync_cup_from_game: type=%u race=%u actors=%d",
              s_selected_game_type, s_race_within_series,
              td5_game_get_total_actor_count());
}

int td5_save_sync_cup_to_game(int *out_race_within_series)
{
    /* Restore header fields to g_td5 */
    g_td5.game_type                = (TD5_GameType)s_selected_game_type;
    g_td5.race_rule_variant        = (int)s_race_rule_variant;
    g_td5.time_trial_enabled       = (int)s_time_trial_enabled;
    g_td5.wanted_mode_enabled      = (int)s_wanted_enabled;
    g_td5.difficulty                = (TD5_Difficulty)s_difficulty_tier;
    g_td5.checkpoint_timers_enabled = (int)s_checkpoint_mode;
    g_td5.traffic_enabled          = (int)s_traffic_enabled;
    g_td5.special_encounter_enabled = (int)s_special_encounter;
    g_td5.circuit_lap_count        = (int)s_circuit_lap_count;

    if (out_race_within_series) {
        *out_race_within_series = (int)s_race_within_series;
    }

    /* TD5RE divergent overlay -> g_td5 push. When the loaded file was an
     * extended-format CupData.td5, push the persisted car indices into
     * the runtime selection so InitRace's td5_asset_load_vehicle pass
     * resolves the same cars the player saved. Legacy files leave g_td5
     * untouched (existing fields keep whatever the frontend chose). */
    if (s_overlay_present) {
        g_td5.car_index             = s_overlay_car_indices[0];
        g_td5.ai_car_indices[1]     = s_overlay_car_indices[1];
        g_td5.ai_car_indices[2]     = s_overlay_car_indices[2];
        g_td5.ai_car_indices[3]     = s_overlay_car_indices[3];
        g_td5.ai_car_indices[4]     = s_overlay_car_indices[4];
        g_td5.ai_car_indices[5]     = s_overlay_car_indices[5];
        TD5_LOG_I(LOG_TAG,
                  "sync_cup_to_game: overlay -> g_td5 car=%d ai=[%d,%d,%d,%d,%d]",
                  g_td5.car_index,
                  g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
                  g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
                  g_td5.ai_car_indices[5]);
    }

    /* Restore actor data to game module actor table. */
    {
        int total = td5_game_get_total_actor_count();
        int i;
        for (i = 0; i < total && i < 14; i++) {
            TD5_Actor *a = td5_game_get_actor(i);
            if (a) {
                memcpy(a, (uint8_t *)s_actor_table + (size_t)i * 0x388,
                       0x388);
            }
        }
    }

    TD5_LOG_I(LOG_TAG, "sync_cup_to_game: type=%u race=%u",
              s_selected_game_type, s_race_within_series);

    return (int)s_selected_game_type;
}

/* Legacy CupData.td5 integrity self-check (open -> decrypt -> CRC at +0x0C).
 * Used only to decide whether a not-yet-migrated binary cup save is offerable. */
static int cup_is_binary_valid(const char *path)
{
    const char *filepath = path ? path : TD5_CUPDATA_FILENAME;

    TD5_File *f = td5_plat_file_open(filepath, "rb");
    if (!f) {
        return 0;
    }

    uint8_t read_buf[TD5_CUPDATA_READ_BUF_SIZE];
    size_t bytes_read = td5_plat_file_read(f, read_buf, TD5_CUPDATA_READ_BUF_SIZE);
    td5_plat_file_close(f);

    if (bytes_read < 0x10) {
        return 0; /* Too small to contain header + CRC */
    }

    /* Decrypt */
    td5_save_xor_decrypt(read_buf, bytes_read, TD5_CUPDATA_XOR_KEY);

    /* Extract stored CRC from offset 0x0C, set placeholder, recompute */
    uint32_t stored_crc = read_le32(read_buf + 0x0C);
    write_le32(read_buf + 0x0C, TD5_CRC_PLACEHOLDER);
    uint32_t computed_crc = td5_save_crc32(read_buf, bytes_read);

    return (stored_crc == computed_crc) ? 1 : 0;
}

int td5_save_is_cup_valid(const char *path)
{
    /* A continuable cup exists if either the new td5re_cup.ini parses with a
     * valid game type, or a (not-yet-migrated) legacy CupData.td5 is intact. */
    const char *cup_ini = path ? path : cfgini_cup_path();

    if (td5_plat_file_exists(cup_ini)) {
        int gt = td5_plat_ini_get_int(cup_ini, "Cup", "GameType", 0);
        if (gt > 0) {
            TD5_LOG_I(LOG_TAG, "is_cup_valid: %s game_type=%d -> valid", cup_ini, gt);
            return 1;
        }
    }
    if (!path && cup_is_binary_valid(NULL)) {
        TD5_LOG_I(LOG_TAG, "is_cup_valid: legacy CupData.td5 intact -> valid (will migrate)");
        return 1;
    }
    return 0;
}

/* ========================================================================
 * Organized human-readable INI persistence (Config.td5 / CupData.td5 retired)
 *
 * Settings continue to live in td5re.ini (read by main.c). The data that
 * used to be locked inside the encrypted binaries is written here as plain,
 * editable INI:
 *   td5re_input.ini     -- bindings + device/FF config
 *   td5re_progress.ini  -- high-score table + unlock/cheat/progression state
 *   td5re_cup.ini       -- "Continue Cup" resume state (no actor snapshot)
 * Files are written in one shot via raw I/O (so we control formatting and
 * comments) and read back through the Win32 profile API.
 * ======================================================================== */

/* --- exe-relative path caches --- */
static const char *cfgini_input_path(void)
{
    static char p[TD5_CFGINI_PATH_MAX]; static int done = 0;
    if (!done) { td5_plat_ini_resolve_path(TD5RE_INPUT_INI, p, sizeof p); done = 1; }
    return p;
}
static const char *cfgini_progress_path(void)
{
    static char p[TD5_CFGINI_PATH_MAX]; static int done = 0;
    if (!done) { td5_plat_ini_resolve_path(TD5RE_PROGRESS_INI, p, sizeof p); done = 1; }
    return p;
}
static const char *cfgini_cup_path(void)
{
    static char p[TD5_CFGINI_PATH_MAX]; static int done = 0;
    if (!done) { td5_plat_ini_resolve_path(TD5RE_CUP_INI, p, sizeof p); done = 1; }
    return p;
}

/* Canonical 10-action keyboard order (mirrors s_kb_bindings in
 * td5_platform_win32.c: LEFT RIGHT ACCEL BRAKE HBRK HORN GUP GDN VIEW REAR). */
static const char *const k_cfgini_kb_names[10] = {
    "Left", "Right", "Accelerate", "Brake", "Handbrake",
    "Horn", "GearUp", "GearDown", "ChangeView", "LookBack"
};

/* --- tiny one-shot INI text builder --- */
typedef struct { char *buf; size_t cap; size_t len; int ok; } CfgIniBuf;

static void cfgini_add(CfgIniBuf *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void cfgini_add(CfgIniBuf *w, const char *fmt, ...)
{
    if (!w->ok || w->len >= w->cap) { w->ok = 0; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) { w->ok = 0; w->len = w->cap; return; }
    w->len += (size_t)n;
}

static int cfgini_flush(CfgIniBuf *w, const char *path)
{
    if (!w->ok) {
        TD5_LOG_W(LOG_TAG, "cfgini_flush: text buffer overflow for %s", path);
        return 0;
    }
    TD5_File *f = td5_plat_file_open(path, "wb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "cfgini_flush: cannot open %s for write", path);
        return 0;
    }
    size_t wr = td5_plat_file_write(f, w->buf, w->len);
    td5_plat_file_close(f);
    /* Invalidate the profile cache so a later read sees these bytes. */
    td5_plat_ini_flush(path);
    return (wr == w->len) ? 1 : 0;
}

/* --- signed / unsigned readers that tolerate negatives + full uint32 ---
 * (GetPrivateProfileInt clamps negative values to 0, so anything that can be
 *  negative or exceed INT_MAX is parsed from the raw string instead.) */
static int cfgini_get_i32(const char *file, const char *sec,
                          const char *key, int fallback)
{
    char tmp[32];
    if (td5_plat_ini_get_str(file, sec, key, "", tmp, sizeof tmp) <= 0) return fallback;
    char *end = NULL;
    long v = strtol(tmp, &end, 0);
    return (end == tmp) ? fallback : (int)v;
}
static uint32_t cfgini_get_u32(const char *file, const char *sec,
                               const char *key, uint32_t fallback)
{
    char tmp[32];
    if (td5_plat_ini_get_str(file, sec, key, "", tmp, sizeof tmp) <= 0) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(tmp, &end, 0);
    return (end == tmp) ? fallback : (uint32_t)v;
}

/* Parse a comma/space separated list of small ints into a uint8_t array.
 * Entries past the end of the text are left untouched (caller pre-fills). */
static void cfgini_parse_u8_list(const char *s, uint8_t *out, int n)
{
    int i = 0;
    const char *p = s;
    while (i < n && p && *p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 0);
        if (end == p) break;
        out[i++] = (uint8_t)v;
        p = end;
    }
}

/* ----------------------------- input bindings ----------------------------- */

static int cfgini_write_input(void)
{
    static char buf[8192];
    CfgIniBuf w = { buf, sizeof buf, 0, 1 };
    const uint8_t *p1kb = (const uint8_t *)s_p1_custom_bindings;
    const uint8_t *p2kb = (const uint8_t *)s_p2_custom_bindings;

    cfgini_add(&w, "; td5re_input.ini -- TD5RE controller + keyboard bindings.\r\n");
    cfgini_add(&w, "; Replaces the binding data formerly locked inside Config.td5.\r\n");
    cfgini_add(&w, "; Scancodes are DirectInput DIK_* codes (decimal or 0x hex).\r\n\r\n");

    cfgini_add(&w, "[Devices]\r\n");
    cfgini_add(&w, "; 0 = keyboard, >=1 = 1-based enumerated joystick index\r\n");
    cfgini_add(&w, "; Player1..Player9 (N-way split-screen). Player1/2 also live in Config.td5.\r\n");
    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++)
        cfgini_add(&w, "Player%d = %u\r\n", pl + 1, (unsigned)s_player_device_index[pl]);
    cfgini_add(&w, "\r\n");

    cfgini_add(&w, "[ForceFeedback]\r\n");
    cfgini_add(&w, "; Raw force-feedback config dwords (Config.td5 +0x22..0x2D)\r\n");
    cfgini_add(&w, "A = %d\r\nB = %d\r\nC = %d\r\nD = %d\r\n\r\n",
               s_ff_config[0], s_ff_config[1], s_ff_config[2], s_ff_config[3]);

    /* [S05 2026-06-04] CATCHUP / rubber-band assist level (orig 0x465FF8). The
     * Multiplayer Options toggle persists it here; the retired Config.td5 binary
     * path (config_serialize_to_buffer) is no longer written, so this organized
     * INI key is the only persistence across runs. 0 = off, 1..9 = on (default
     * 1). The td5re.ini [GameOptions] CatchupAssist key (-1 = use this value) is a
     * separate power-user override resolved by td5_ai_get_catchup_level(). */
    cfgini_add(&w, "[Assist]\r\n");
    cfgini_add(&w, "Catchup = %u\r\n\r\n", (unsigned)td5_save_get_catchup_assist());

    cfgini_add(&w, "[ControllerButtons]\r\n");
    cfgini_add(&w, "; raw joystick binding dwords [player*9 + slot], players 1..9:\r\n");
    cfgini_add(&w, ";   slot0 = active flag, slot1/2 = axis assignment, slot3..8 = button actions.\r\n");
    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++)
        for (int sl = 0; sl < 9; sl++)
            cfgini_add(&w, "P%d_%d = %u\r\n", pl + 1, sl,
                       (unsigned)s_controller_bindings[pl * 9 + sl]);
    cfgini_add(&w, "\r\n");

    cfgini_add(&w, "[ControllerActions]\r\n");
    cfgini_add(&w, "; per-action joystick binding codes [player][action 0..9]:\r\n");
    cfgini_add(&w, ";   0=unbound, 0x100|btn = button, 0x200|(axis<<1)|dir = axis/trigger.\r\n");
    cfgini_add(&w, ";   actions: 0=LEFT 1=RIGHT 2=ACCEL 3=BRAKE 4=HANDBRAKE 5=HORN\r\n");
    cfgini_add(&w, ";            6=GEARUP 7=GEARDOWN 8=VIEW 9=REARVIEW\r\n");
    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++)
        for (int a = 0; a < TD5_JSBIND_ACTIONS; a++)
            cfgini_add(&w, "A%d_%d = %u\r\n", pl + 1, a,
                       (unsigned)s_js_action_bind[pl * TD5_JSBIND_ACTIONS + a]);
    cfgini_add(&w, "\r\n");

    cfgini_add(&w, "[KeyboardPlayer1]\r\n; DirectInput scancodes. 0 = unbound (keeps the built-in default).\r\n");
    for (int i = 0; i < 10; i++)
        cfgini_add(&w, "%s = %u\r\n", k_cfgini_kb_names[i], (unsigned)p1kb[i]);
    cfgini_add(&w, "\r\n[KeyboardPlayer2]\r\n");
    for (int i = 0; i < 10; i++)
        cfgini_add(&w, "%s = %u\r\n", k_cfgini_kb_names[i], (unsigned)p2kb[i]);

    return cfgini_flush(&w, cfgini_input_path());
}

static int cfgini_read_input(void)
{
    const char *f = cfgini_input_path();
    if (!td5_plat_file_exists(f)) return 0;
    uint8_t *p1kb = (uint8_t *)s_p1_custom_bindings;
    uint8_t *p2kb = (uint8_t *)s_p2_custom_bindings;
    char key[8];

    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++) {
        char dkey[12];
        snprintf(dkey, sizeof dkey, "Player%d", pl + 1);
        s_player_device_index[pl] = (uint32_t)td5_plat_ini_get_int(
            f, "Devices", dkey, (int)s_player_device_index[pl]);
    }
    s_p1_device_index = s_player_device_index[0];   /* keep legacy fields in sync */
    s_p2_device_index = s_player_device_index[1];

    s_ff_config[0] = cfgini_get_i32(f, "ForceFeedback", "A", s_ff_config[0]);
    s_ff_config[1] = cfgini_get_i32(f, "ForceFeedback", "B", s_ff_config[1]);
    s_ff_config[2] = cfgini_get_i32(f, "ForceFeedback", "C", s_ff_config[2]);
    s_ff_config[3] = cfgini_get_i32(f, "ForceFeedback", "D", s_ff_config[3]);

    /* [S05 2026-06-04] CATCHUP / rubber-band assist (set clamps to 0..9). */
    td5_save_set_catchup_assist(
        td5_plat_ini_get_int(f, "Assist", "Catchup", td5_save_get_catchup_assist()));

    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++)
        for (int sl = 0; sl < 9; sl++) {
            snprintf(key, sizeof key, "P%d_%d", pl + 1, sl);
            s_controller_bindings[pl * 9 + sl] =
                cfgini_get_u32(f, "ControllerButtons", key, s_controller_bindings[pl * 9 + sl]);
        }

    for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++)
        for (int a = 0; a < TD5_JSBIND_ACTIONS; a++) {
            snprintf(key, sizeof key, "A%d_%d", pl + 1, a);
            s_js_action_bind[pl * TD5_JSBIND_ACTIONS + a] =
                cfgini_get_u32(f, "ControllerActions", key,
                               s_js_action_bind[pl * TD5_JSBIND_ACTIONS + a]);
        }

    /* [FIX 2026-07-04] Retroactively un-stick joystick rows that were frozen
     * verbatim onto a now-superseded built-in default. Before this fix,
     * merely opening Controller Setup for an unconfigured joystick and
     * pressing OK (even without touching a single binding) permanently saved
     * a snapshot of whatever k_default_js_action_bind was that day — so a
     * player who visited early keeps an old CHANGE VIEW button forever while
     * every joystick nobody ever visited tracks the live (current) default.
     * That produced exactly "joystick 1 has a different camera button than
     * the others." The behavioural fix (Screen_ControllerBinding, td5_fe_menu.c)
     * stops NEW freezes; this repairs installs that already have one, by
     * recognizing an EXACT byte-for-byte match against a historical default
     * snapshot (not a real user edit — no genuine remap would coincidentally
     * reproduce 11 stock values) and resetting the row to unconfigured so it
     * resumes tracking the live default. RE basis: N/A — TD5RE-only, no
     * original counterpart (k_default_js_action_bind is a port invention). */
    {
        /* Gen A (before commit 75c1d767, 2026-06-24): CHANGE VIEW = button 6. */
        static const uint32_t k_stale_gen_a[TD5_JSBIND_ACTIONS] = {
            TD5_JSBIND_AXIS | (0u << 1) | 1u, TD5_JSBIND_AXIS | (0u << 1) | 0u,
            TD5_JSBIND_AXIS | (2u << 1) | 1u, TD5_JSBIND_AXIS | (2u << 1) | 0u,
            TD5_JSBIND_BUTTON | 2u, TD5_JSBIND_BUTTON | 3u,
            TD5_JSBIND_BUTTON | 5u, TD5_JSBIND_BUTTON | 4u,
            TD5_JSBIND_BUTTON | 6u, TD5_JSBIND_BUTTON | 9u, TD5_JSBIND_BUTTON | 7u,
        };
        /* Gen B (2026-06-24 .. before 03686cf3, 2026-06-27): CHANGE VIEW = button 8 (L3). */
        static const uint32_t k_stale_gen_b[TD5_JSBIND_ACTIONS] = {
            TD5_JSBIND_AXIS | (0u << 1) | 1u, TD5_JSBIND_AXIS | (0u << 1) | 0u,
            TD5_JSBIND_AXIS | (2u << 1) | 1u, TD5_JSBIND_AXIS | (2u << 1) | 0u,
            TD5_JSBIND_BUTTON | 2u, TD5_JSBIND_BUTTON | 3u,
            TD5_JSBIND_BUTTON | 5u, TD5_JSBIND_BUTTON | 4u,
            TD5_JSBIND_BUTTON | 8u, TD5_JSBIND_BUTTON | 9u, TD5_JSBIND_BUTTON | 7u,
        };
        for (int pl = 0; pl < TD5_MAX_HUMAN_PLAYERS; pl++) {
            uint32_t *row = &s_js_action_bind[pl * TD5_JSBIND_ACTIONS];
            int is_stale = (memcmp(row, k_stale_gen_a, sizeof(k_stale_gen_a)) == 0) ||
                           (memcmp(row, k_stale_gen_b, sizeof(k_stale_gen_b)) == 0);
            if (is_stale) {
                TD5_LOG_I(LOG_TAG,
                    "Input migration: player %d joystick action row matched a "
                    "superseded built-in default (frozen CHANGE VIEW button) — "
                    "clearing so it resumes tracking the live default", pl + 1);
                memset(row, 0, TD5_JSBIND_ACTIONS * sizeof(uint32_t));
            }
        }
    }

    for (int i = 0; i < 10; i++) {
        p1kb[i] = (uint8_t)td5_plat_ini_get_int(f, "KeyboardPlayer1", k_cfgini_kb_names[i], p1kb[i]);
        p2kb[i] = (uint8_t)td5_plat_ini_get_int(f, "KeyboardPlayer2", k_cfgini_kb_names[i], p2kb[i]);
    }

    /* Mirror config_deserialize_from_buffer: push the loaded scancodes down to
     * the live input layer so saved rebinds take effect in-race. */
    td5_plat_input_set_keyboard_bindings(0, p1kb, 10);
    td5_plat_input_set_keyboard_bindings(1, p2kb, 10);
    return 1;
}

/* --------------------- high scores + unlock/progression -------------------- */

static int cfgini_write_progress(void)
{
    static char buf[96 * 1024];
    CfgIniBuf w = { buf, sizeof buf, 0, 1 };
    const TD5_NpcGroup *groups = (const TD5_NpcGroup *)s_npc_group_table;

    cfgini_add(&w, "; td5re_progress.ini -- TD5RE high scores + unlock/progression state.\r\n");
    cfgini_add(&w, "; Replaces the Config.td5 high-score table, lock tables and cheat flags.\r\n\r\n");

    cfgini_add(&w, "[Unlocks]\r\n");
    cfgini_add(&w, "MaxUnlockedCar = %u\r\n", (unsigned)s_max_unlocked_car);
    cfgini_add(&w, "AllCarsUnlocked = %u\r\n", (unsigned)s_all_cars_unlocked);
    cfgini_add(&w, "CupTier = %u\r\n", (unsigned)(s_cup_tier & 0x07));
    cfgini_add(&w, "; CarLocks: %d entries, 0 = unlocked, 1 = locked\r\n", TD5_CONFIG_NUM_CARS);
    cfgini_add(&w, "CarLocks =");
    for (int i = 0; i < TD5_CONFIG_NUM_CARS; i++)
        cfgini_add(&w, "%s%u", i ? "," : " ", (unsigned)s_car_locks[i]);
    cfgini_add(&w, "\r\n");
    cfgini_add(&w, "; TrackLocks: %d entries, 1 = unlocked, 0 = locked (internal sense)\r\n", TD5_CONFIG_NUM_TRACKS);
    cfgini_add(&w, "TrackLocks =");
    for (int i = 0; i < TD5_CONFIG_NUM_TRACKS; i++)
        cfgini_add(&w, "%s%u", i ? "," : " ", (unsigned)s_track_locks[i]);
    cfgini_add(&w, "\r\n\r\n");

    cfgini_add(&w, "[Cheats]\r\n; %d flags, 0/1 (only bit 0 is persisted)\r\nFlags =", TD5_CONFIG_NUM_CHEATS);
    for (int i = 0; i < TD5_CONFIG_NUM_CHEATS; i++)
        cfgini_add(&w, "%s%u", i ? "," : " ", (unsigned)(s_cheat_flags[i] & 1));
    cfgini_add(&w, "\r\n\r\n");

    cfgini_add(&w, "[Audio]\r\nMusicTrack = %u\r\n\r\n", (unsigned)s_music_track);

    cfgini_add(&w, "[Tutorial]\r\n; 1 once the first-race controller-tutorial overlay was dismissed.\r\nSeen = %u\r\n\r\n",
               (unsigned)(s_tutorial_seen ? 1 : 0));

    cfgini_add(&w, "; High scores: 26 tracks x 5 entries. Header selects the score column:\r\n");
    cfgini_add(&w, ";   0=TIME(MM:SS.cc) 1=LAP 2=PTS(points) 4=TIME(MM:SS.mmm)\r\n");
    cfgini_add(&w, "; Score = raw stored value (ticks @30fps for time types, points for PTS).\r\n\r\n");
    for (int g = 0; g < TD5_CONFIG_NPC_GROUPS; g++) {
        const TD5_NpcGroup *grp = &groups[g];
        cfgini_add(&w, "[HighScores.Track%02d]\r\n", g);
        cfgini_add(&w, "Header = %d\r\n", grp->header);
        for (int e = 0; e < 5; e++) {
            const TD5_NpcEntry *en = &grp->entries[e];
            char nm[17];
            memcpy(nm, en->name, 16);
            nm[16] = '\0';
            /* Defensively strip any control byte so it cannot break the INI. */
            for (int c = 0; c < 16; c++) {
                if ((unsigned char)nm[c] < 0x20) { nm[c] = '\0'; break; }
            }
            cfgini_add(&w, "Entry%d.Name = %s\r\n", e, nm);
            cfgini_add(&w, "Entry%d.Score = %d\r\n", e, en->score);
            cfgini_add(&w, "Entry%d.Car = %d\r\n", e, en->car_id);
            cfgini_add(&w, "Entry%d.AvgSpeed = %d\r\n", e, en->avg_speed);
            cfgini_add(&w, "Entry%d.TopSpeed = %d\r\n", e, en->top_speed);
            /* TD5RE extension: full display name + race-results-parity metrics. */
            {
                const TD5_NpcEntryExt *ex = &s_npc_ext[g][e];
                if (ex->full_name[0]) {
                    char fn[32]; snprintf(fn, sizeof fn, "%s", ex->full_name);
                    for (int c = 0; c < (int)sizeof(fn); c++)
                        if ((unsigned char)fn[c] < 0x20) { fn[c] = '\0'; break; }
                    cfgini_add(&w, "Entry%d.FullName = %s\r\n", e, fn);
                }
                cfgini_add(&w, "Entry%d.Collisions = %d\r\n", e, ex->collisions);
                cfgini_add(&w, "Entry%d.Air = %d\r\n", e, ex->air_ticks);
            }
        }
        cfgini_add(&w, "\r\n");
    }

    /* [#11] Player profiles -- persistent name + colour + car presets. Written
     * after the high-score block so they share this one organized file. Ensure
     * the store is loaded first: an external td5_save_write_config() (e.g. a
     * rebind save) must not clobber on-disk profiles with an empty in-memory
     * set just because the profile API has not been touched yet this run. */
    profiles_ensure_loaded();
    cfgini_add(&w, "[Profiles]\r\n");
    cfgini_add(&w, "; Saved player presets for the multiplayer name/colour screen.\r\n");
    cfgini_add(&w, "Count = %d\r\n", s_profile_count);
    for (int i = 0; i < s_profile_count; i++) {
        const TD5_Profile *pr = &s_profiles[i];
        char nm[17];
        memcpy(nm, pr->name, 16);
        nm[16] = '\0';
        /* Defensively strip any control byte so it cannot break the INI. */
        for (int c = 0; c < 16; c++) {
            if ((unsigned char)nm[c] < 0x20) { nm[c] = '\0'; break; }
        }
        cfgini_add(&w, "Profile%dName = %s\r\n", i, nm);
        cfgini_add(&w, "Profile%dAccent = %d\r\n", i, pr->accent);
        cfgini_add(&w, "Profile%dCar = %d\r\n", i, pr->car);
        cfgini_add(&w, "Profile%dPaint = %d\r\n", i, pr->paint);
        cfgini_add(&w, "Profile%dColor = %d\r\n", i, pr->color);
        cfgini_add(&w, "Profile%dTrans = %d\r\n", i, pr->trans);
    }
    cfgini_add(&w, "\r\n");

    /* [#2b] TD6 per-track high-score records — genuine player runs only. Same
     * entry layout as the TD5 high-score groups; header -1 = no records yet, so
     * those levels are skipped here (nothing to persist). Loaded first so an
     * unrelated write (e.g. a rebind save) can't drop on-disk TD6 records. */
    td6_records_ensure_loaded();
    cfgini_add(&w, "; TD6 high-score records: genuine player runs per TD6 level\r\n");
    cfgini_add(&w, "; (no authored/seed names). Header: 0=TIME 1=LAP 2=PTS 4=TIME(ms).\r\n\r\n");
    for (int lv = 0; lv < TD5_MAX_TD6_RECORD_LEVELS; lv++) {
        const TD5_NpcGroup *grp = &s_td6_records[lv];
        if (grp->header < 0) continue;        /* no records for this level */
        cfgini_add(&w, "[TD6Records.Level%02d]\r\n", lv);
        cfgini_add(&w, "Header = %d\r\n", grp->header);
        for (int e = 0; e < 5; e++) {
            const TD5_NpcEntry *en = &grp->entries[e];
            char nm[17];
            memcpy(nm, en->name, 16);
            nm[16] = '\0';
            for (int c = 0; c < 16; c++) {
                if ((unsigned char)nm[c] < 0x20) { nm[c] = '\0'; break; }
            }
            cfgini_add(&w, "Entry%d.Name = %s\r\n", e, nm);
            cfgini_add(&w, "Entry%d.Score = %d\r\n", e, en->score);
            cfgini_add(&w, "Entry%d.Car = %d\r\n", e, en->car_id);
            cfgini_add(&w, "Entry%d.AvgSpeed = %d\r\n", e, en->avg_speed);
            cfgini_add(&w, "Entry%d.TopSpeed = %d\r\n", e, en->top_speed);
            /* TD5RE extension: full display name + race-results-parity metrics. */
            {
                const TD5_NpcEntryExt *ex = &s_td6_ext[lv][e];
                if (ex->full_name[0]) {
                    char fn[32]; snprintf(fn, sizeof fn, "%s", ex->full_name);
                    for (int c = 0; c < (int)sizeof(fn); c++)
                        if ((unsigned char)fn[c] < 0x20) { fn[c] = '\0'; break; }
                    cfgini_add(&w, "Entry%d.FullName = %s\r\n", e, fn);
                }
                cfgini_add(&w, "Entry%d.Collisions = %d\r\n", e, ex->collisions);
                cfgini_add(&w, "Entry%d.Air = %d\r\n", e, ex->air_ticks);
            }
        }
        cfgini_add(&w, "\r\n");
    }

    return cfgini_flush(&w, cfgini_progress_path());
}

static int cfgini_read_progress(void)
{
    const char *f = cfgini_progress_path();
    if (!td5_plat_file_exists(f)) return 0;
    char val[512];
    char key[24];

    s_max_unlocked_car  = (uint32_t)td5_plat_ini_get_int(f, "Unlocks", "MaxUnlockedCar", (int)s_max_unlocked_car);
    s_all_cars_unlocked = (uint8_t)td5_plat_ini_get_int(f, "Unlocks", "AllCarsUnlocked", s_all_cars_unlocked);
    s_cup_tier          = (uint32_t)(td5_plat_ini_get_int(f, "Unlocks", "CupTier", (int)s_cup_tier) & 0x07);

    if (td5_plat_ini_get_str(f, "Unlocks", "CarLocks", "", val, sizeof val) > 0)
        cfgini_parse_u8_list(val, s_car_locks, TD5_CONFIG_NUM_CARS);
    if (td5_plat_ini_get_str(f, "Unlocks", "TrackLocks", "", val, sizeof val) > 0)
        cfgini_parse_u8_list(val, s_track_locks, TD5_CONFIG_NUM_TRACKS);
    if (td5_plat_ini_get_str(f, "Cheats", "Flags", "", val, sizeof val) > 0)
        cfgini_parse_u8_list(val, s_cheat_flags, TD5_CONFIG_NUM_CHEATS);

    s_music_track = (uint32_t)td5_plat_ini_get_int(f, "Audio", "MusicTrack", (int)s_music_track);
    s_tutorial_seen = td5_plat_ini_get_int(f, "Tutorial", "Seen", s_tutorial_seen);

    TD5_NpcGroup *groups = (TD5_NpcGroup *)s_npc_group_table;
    for (int g = 0; g < TD5_CONFIG_NPC_GROUPS; g++) {
        char sec[24];
        snprintf(sec, sizeof sec, "HighScores.Track%02d", g);
        TD5_NpcGroup *grp = &groups[g];
        grp->header = cfgini_get_i32(f, sec, "Header", grp->header);
        for (int e = 0; e < 5; e++) {
            TD5_NpcEntry *en = &grp->entries[e];
            snprintf(key, sizeof key, "Entry%d.Name", e);
            if (td5_plat_ini_get_str(f, sec, key, "", val, sizeof val) > 0) {
                size_t nlen = strlen(val);
                if (nlen > 15) nlen = 15;       /* name field is 16B incl NUL */
                memset(en->name, 0, 16);
                memcpy(en->name, val, nlen);
            }
            snprintf(key, sizeof key, "Entry%d.Score", e);    en->score     = cfgini_get_i32(f, sec, key, en->score);
            snprintf(key, sizeof key, "Entry%d.Car", e);      en->car_id    = cfgini_get_i32(f, sec, key, en->car_id);
            snprintf(key, sizeof key, "Entry%d.AvgSpeed", e); en->avg_speed = cfgini_get_i32(f, sec, key, en->avg_speed);
            snprintf(key, sizeof key, "Entry%d.TopSpeed", e); en->top_speed = cfgini_get_i32(f, sec, key, en->top_speed);
            /* TD5RE extension: full display name + collisions + air time. */
            {
                TD5_NpcEntryExt *ex = &s_npc_ext[g][e];
                snprintf(key, sizeof key, "Entry%d.FullName", e);
                if (td5_plat_ini_get_str(f, sec, key, "", val, sizeof val) > 0) {
                    memset(ex->full_name, 0, sizeof(ex->full_name));
                    strncpy(ex->full_name, val, sizeof(ex->full_name) - 1);
                }
                snprintf(key, sizeof key, "Entry%d.Collisions", e); ex->collisions = cfgini_get_i32(f, sec, key, ex->collisions);
                snprintf(key, sizeof key, "Entry%d.Air", e);        ex->air_ticks  = cfgini_get_i32(f, sec, key, ex->air_ticks);
            }
        }
    }

    /* [#11] Player profiles share this file -- load them in the same pass. */
    profiles_read();
    /* [#2b] TD6 records share this file too. */
    td6_records_read();
    return 1;
}

/* ------------------------------ player profiles ---------------------------- */

/* Cached TD5RE_PROFILES knob: 0 disables the persistent profile store (the API
 * still works in-memory but never reads/writes disk). Default on. Resolved +
 * logged once. */
static int profiles_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_PROFILES");
        cached = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "player profiles %s (TD5RE_PROFILES=%s)",
                  cached ? "enabled" : "disabled", e ? e : "(unset)");
    }
    return cached;
}

/* Read the [Profiles] section of td5re_progress.ini into the static store.
 * Tolerates a missing file / absent section (leaves count 0). */
static void profiles_read(void)
{
    s_profile_count = 0;
    memset(s_profiles, 0, sizeof(s_profiles));
    if (!profiles_enabled()) return;

    const char *f = cfgini_progress_path();
    if (!td5_plat_file_exists(f)) return;

    int n = td5_plat_ini_get_int(f, "Profiles", "Count", 0);
    if (n < 0) n = 0;
    if (n > TD5_MAX_PROFILES) n = TD5_MAX_PROFILES;

    char val[64];
    char key[24];
    int out = 0;
    for (int i = 0; i < n; i++) {
        TD5_Profile *pr = &s_profiles[out];
        snprintf(key, sizeof key, "Profile%dName", i);
        if (td5_plat_ini_get_str(f, "Profiles", key, "", val, sizeof val) <= 0)
            continue;                       /* skip nameless / missing slots */
        size_t nlen = strlen(val);
        if (nlen > 15) nlen = 15;           /* name field is 16B incl NUL */
        if (nlen == 0) continue;
        memset(pr->name, 0, sizeof pr->name);
        memcpy(pr->name, val, nlen);
        snprintf(key, sizeof key, "Profile%dAccent", i); pr->accent = cfgini_get_i32(f, "Profiles", key, 0);
        snprintf(key, sizeof key, "Profile%dCar", i);    pr->car    = cfgini_get_i32(f, "Profiles", key, 0);
        snprintf(key, sizeof key, "Profile%dPaint", i);  pr->paint  = cfgini_get_i32(f, "Profiles", key, 0);
        snprintf(key, sizeof key, "Profile%dColor", i);  pr->color  = cfgini_get_i32(f, "Profiles", key, 0);
        snprintf(key, sizeof key, "Profile%dTrans", i);  pr->trans  = cfgini_get_i32(f, "Profiles", key, 0);
        out++;
    }
    s_profile_count = out;
}

/* Lazily load the profile store the first time the API is touched. Loading is
 * gated separately from td5_save_init's progress read so the profile API works
 * even if a caller reaches it before init (count 0 on a missing store). */
static void profiles_ensure_loaded(void)
{
    if (s_profiles_loaded) return;
    s_profiles_loaded = 1;
    profiles_read();
}

int td5_save_profile_count(void)
{
    profiles_ensure_loaded();
    return s_profile_count;
}

int td5_save_profile_get(int idx, TD5_Profile *out)
{
    profiles_ensure_loaded();
    if (!out || idx < 0 || idx >= s_profile_count) return 0;
    *out = s_profiles[idx];
    return 1;
}

int td5_save_profile_save(const TD5_Profile *p)
{
    profiles_ensure_loaded();
    if (!p) return -1;

    /* Trim the incoming name to a clean 16B field (NUL-padded, no control
     * bytes); reject empties -- the name is the upsert key. */
    char name[16];
    memset(name, 0, sizeof name);
    {
        int j = 0;
        for (int i = 0; i < 16 && p->name[i]; i++) {
            if ((unsigned char)p->name[i] < 0x20) break;
            name[j++] = p->name[i];
        }
    }
    if (name[0] == '\0') return -1;

    /* UPSERT by name (case-insensitive). Both fields are exactly 16 bytes;
     * _strnicmp with n=16 reads no further even when a full-length name has no
     * NUL terminator. */
    int slot = -1;
    for (int i = 0; i < s_profile_count; i++) {
        if (_strnicmp(s_profiles[i].name, name, 16) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_profile_count >= TD5_MAX_PROFILES) return -1;   /* store full */
        slot = s_profile_count++;
    }

    s_profiles[slot] = *p;
    memcpy(s_profiles[slot].name, name, 16);   /* store the cleaned name */

    if (profiles_enabled())
        cfgini_write_progress();               /* persist whole progress file */
    return slot;
}

int td5_save_profile_delete(int idx)
{
    profiles_ensure_loaded();
    if (idx < 0 || idx >= s_profile_count) return 0;

    for (int i = idx; i < s_profile_count - 1; i++)
        s_profiles[i] = s_profiles[i + 1];
    s_profile_count--;
    memset(&s_profiles[s_profile_count], 0, sizeof(s_profiles[0]));

    if (profiles_enabled())
        cfgini_write_progress();
    return 1;
}

/* ---- First-race tutorial overlay "seen" flag (PORT ENHANCEMENT 2026-06) ---- */

int td5_save_get_tutorial_seen(void)
{
    return s_tutorial_seen ? 1 : 0;
}

void td5_save_set_tutorial_seen(int seen)
{
    int v = seen ? 1 : 0;
    if (s_tutorial_seen == v) return;     /* no change → no disk write */
    s_tutorial_seen = v;
    cfgini_write_progress();              /* persist immediately */
    TD5_LOG_I(LOG_TAG, "Tutorial seen flag persisted: %d", v);
}

/* ---------------------------- TD6 high-score records ----------------------- */

/* Cached TD5RE_TD6_NO_PLACEHOLDER_SCORES knob: when ON (default) TD6 tracks use
 * this genuine-records store instead of clamping onto a TD5 NPC group's fake
 * names. "0" disables it (returns NULL/-1 so the legacy clamp path renders the
 * old placeholder list). Resolved + logged once. */
static int td6_records_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_TD6_NO_PLACEHOLDER_SCORES");
        cached = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "TD6 genuine high-score records %s (TD5RE_TD6_NO_PLACEHOLDER_SCORES=%s)",
                  cached ? "enabled (no placeholder names)" : "disabled (legacy clamp)",
                  e ? e : "(unset)");
    }
    return cached;
}

/* Read the [TD6Records.LevelNN] sections of td5re_progress.ini into the store.
 * Tolerates a missing file / absent sections (those levels stay header -1). */
static void td6_records_read(void)
{
    for (int lv = 0; lv < TD5_MAX_TD6_RECORD_LEVELS; lv++) {
        memset(&s_td6_records[lv], 0, sizeof(s_td6_records[lv]));
        s_td6_records[lv].header = -1;
        memset(&s_td6_ext[lv], 0, sizeof(s_td6_ext[lv]));
    }
    if (!td6_records_enabled()) return;

    const char *f = cfgini_progress_path();
    if (!td5_plat_file_exists(f)) return;

    char val[64];
    char sec[28];
    char key[24];
    for (int lv = 0; lv < TD5_MAX_TD6_RECORD_LEVELS; lv++) {
        snprintf(sec, sizeof sec, "TD6Records.Level%02d", lv);
        int hdr = cfgini_get_i32(f, sec, "Header", -1);
        if (hdr < 0) continue;                 /* no records section for this level */
        TD5_NpcGroup *grp = &s_td6_records[lv];
        grp->header = hdr;
        for (int e = 0; e < 5; e++) {
            TD5_NpcEntry *en = &grp->entries[e];
            snprintf(key, sizeof key, "Entry%d.Name", e);
            if (td5_plat_ini_get_str(f, sec, key, "", val, sizeof val) > 0) {
                size_t nlen = strlen(val);
                if (nlen > 15) nlen = 15;       /* name field is 16B incl NUL */
                memset(en->name, 0, 16);
                memcpy(en->name, val, nlen);
            }
            snprintf(key, sizeof key, "Entry%d.Score", e);    en->score     = cfgini_get_i32(f, sec, key, 0);
            snprintf(key, sizeof key, "Entry%d.Car", e);      en->car_id    = cfgini_get_i32(f, sec, key, 0);
            snprintf(key, sizeof key, "Entry%d.AvgSpeed", e); en->avg_speed = cfgini_get_i32(f, sec, key, 0);
            snprintf(key, sizeof key, "Entry%d.TopSpeed", e); en->top_speed = cfgini_get_i32(f, sec, key, 0);
            /* TD5RE extension: full display name + collisions + air time. */
            {
                TD5_NpcEntryExt *ex = &s_td6_ext[lv][e];
                snprintf(key, sizeof key, "Entry%d.FullName", e);
                if (td5_plat_ini_get_str(f, sec, key, "", val, sizeof val) > 0) {
                    memset(ex->full_name, 0, sizeof(ex->full_name));
                    strncpy(ex->full_name, val, sizeof(ex->full_name) - 1);
                }
                snprintf(key, sizeof key, "Entry%d.Collisions", e); ex->collisions = cfgini_get_i32(f, sec, key, 0);
                snprintf(key, sizeof key, "Entry%d.Air", e);        ex->air_ticks  = cfgini_get_i32(f, sec, key, 0);
            }
        }
    }
}

/* Lazily load the TD6 record store the first time the API is touched (mirrors
 * profiles_ensure_loaded so it works even before a config load). */
static void td6_records_ensure_loaded(void)
{
    if (s_td6_records_loaded) return;
    s_td6_records_loaded = 1;
    td6_records_read();
}

/* [#11 TD6 PLACEHOLDER HIGH SCORES 2026-06-19] Placeholder high-score tables for
 * the migrated TD6 tracks — both the P2P cities (Paris/NewYork/Rome/HongKong/
 * London = levels 8..12) AND the circuits (Pelton/Ireland/LakeTahoe/CapeHatteras/
 * Switzerland/Egypt = levels 7,18..22), shown until a genuine run is recorded —
 * names + times to beat, exactly like the TD5 tracks have. Copied from the TD5
 * default NPC groups so the score units, names and cars are all valid, into a
 * SEPARATE buffer so the genuine on-disk store (s_td6_records) is never polluted
 * or persisted with placeholder names. Generalised 2026-06-19 to cover EVERY TD6
 * level so the High Scores browse screen lists every TD6 track like TD5. Disable
 * with TD5RE_TD6_HIGHSCORE_PLACEHOLDERS=0 (then empty tracks just show no records). */
static const TD5_NpcGroup *td6_placeholder_group(int td6_level)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = td5_env_flag_on("TD5RE_TD6_HIGHSCORE_PLACEHOLDERS");
    }
    if (!enabled) return NULL;
    if (td6_level < 0 || td6_level >= TD5_MAX_TD6_RECORD_LEVELS) return NULL;

    static TD5_NpcGroup s_ph[TD5_MAX_TD6_RECORD_LEVELS];
    static int s_ph_ready = 0;
    if (!s_ph_ready) {
        /* s_npc_group_table is a byte buffer laid out as an array of TD5_NpcGroup
         * (stride TD5_CONFIG_NPC_GROUP_SIZE) — cast it the same way the TD5 path
         * does (see line ~1874) rather than index it as raw bytes. */
        const TD5_NpcGroup *groups = (const TD5_NpcGroup *)s_npc_group_table;
        int ngroups = TD5_CONFIG_NPC_GROUPS;
        if (ngroups < 1) ngroups = 1;
        for (int c = 0; c < TD5_MAX_TD6_RECORD_LEVELS; c++) {
            s_ph[c] = groups[c % ngroups];             /* vary names/times per level */
            s_ph[c].header = 0;                        /* time-type table */
        }
        s_ph_ready = 1;
    }
    return &s_ph[td6_level];
}

const TD5_NpcGroup *td5_save_get_td6_record_group(int td6_level)
{
    if (!td6_records_enabled()) return NULL;
    td6_records_ensure_loaded();
    if (td6_level < 0 || td6_level >= TD5_MAX_TD6_RECORD_LEVELS) return NULL;
    if (s_td6_records[td6_level].header < 0)
        return td6_placeholder_group(td6_level);   /* no genuine record -> placeholders */
    return &s_td6_records[td6_level];
}

int td5_save_td6_record_insert(int td6_level, int score_type,
                               const char *name, int32_t score,
                               int car_id, int32_t avg_speed, int32_t top_speed,
                               int32_t collisions, int32_t air_ticks)
{
    if (!td6_records_enabled()) return -1;
    td6_records_ensure_loaded();
    if (td6_level < 0 || td6_level >= TD5_MAX_TD6_RECORD_LEVELS) return -1;
    if (score == 0) return -1;                 /* DNF / no real time = no record */

    TD5_NpcGroup    *grp = &s_td6_records[td6_level];
    TD5_NpcEntryExt *ext = s_td6_ext[td6_level];
    int type = score_type & 3;                 /* 0/1=time, 2=points (4 -> 0) */
    if (grp->header < 0) {                      /* first record for this level */
        grp->header = score_type;
        memset(grp->entries, 0, sizeof(grp->entries));
        memset(ext, 0, sizeof(s_td6_ext[td6_level]));
    }

    /* Find the insert rank, keeping the 5 entries sorted (time: lower is better;
     * points: higher is better). Empty rows (name[0]==0) sort to the bottom. */
    int ins_pos = 5;
    for (int k = 0; k < 5; k++) {
        if (grp->entries[k].name[0] == '\0') { ins_pos = k; break; }  /* fill a gap */
        if (type < 2) { if (score <= grp->entries[k].score) { ins_pos = k; break; } }
        else          { if (score >= grp->entries[k].score) { ins_pos = k; break; } }
    }
    if (ins_pos >= 5) return -1;               /* didn't beat any of the 5 */

    /* Shift lower entries (and their extensions) down one slot, then write. */
    for (int k = 3; k >= ins_pos; k--) {
        grp->entries[k + 1] = grp->entries[k];
        ext[k + 1]          = ext[k];
    }
    TD5_NpcEntry *en = &grp->entries[ins_pos];
    memset(en, 0, sizeof(*en));
    if (name) {
        size_t nlen = strlen(name);
        if (nlen > 15) nlen = 15;
        memcpy(en->name, name, nlen);
    }
    en->score     = score;
    en->car_id    = car_id;
    en->avg_speed = avg_speed;
    en->top_speed = top_speed;

    memset(&ext[ins_pos], 0, sizeof(ext[ins_pos]));
    if (name) strncpy(ext[ins_pos].full_name, name, sizeof(ext[ins_pos].full_name) - 1);
    ext[ins_pos].collisions = collisions;
    ext[ins_pos].air_ticks  = air_ticks;

    cfgini_write_progress();                   /* persist whole progress file */
    TD5_LOG_I(LOG_TAG, "TD6 record inserted: level=%d rank=%d name='%s' score=%d coll=%d air=%d",
              td6_level, ins_pos, en->name, (int)score, (int)collisions, (int)air_ticks);
    return ins_pos;
}

/* ------------------------------- cup resume -------------------------------- */

static int cfgini_write_cup(const char *path)
{
    static char buf[16384];
    CfgIniBuf w = { buf, sizeof buf, 0, 1 };

    cfgini_add(&w, "; td5re_cup.ini -- TD5RE \"Continue Cup\" resume state (replaces CupData.td5).\r\n");
    cfgini_add(&w, "; The per-actor physics snapshot from the original CupData.td5 is intentionally\r\n");
    cfgini_add(&w, "; NOT stored -- the next race re-initialises actors at its own grid.\r\n");
    cfgini_add(&w, "; Delete this file (or set GameType = 0) to discard the saved cup.\r\n\r\n");

    cfgini_add(&w, "[Cup]\r\n");
    cfgini_add(&w, "GameType = %u\r\n", (unsigned)s_selected_game_type);
    cfgini_add(&w, "RaceIndex = %u\r\n", (unsigned)s_race_within_series);
    cfgini_add(&w, "NpcGroupIndex = %u\r\n", (unsigned)s_npc_group_index);
    cfgini_add(&w, "TrackOpponentState = %u\r\n", (unsigned)s_track_opponent_state);
    cfgini_add(&w, "RaceRuleVariant = %u\r\n", (unsigned)s_race_rule_variant);
    cfgini_add(&w, "TimeTrial = %u\r\n", (unsigned)s_time_trial_enabled);
    cfgini_add(&w, "Wanted = %u\r\n", (unsigned)s_wanted_enabled);
    cfgini_add(&w, "Difficulty = %u\r\n", (unsigned)s_difficulty_tier);
    cfgini_add(&w, "CheckpointMode = %u\r\n", (unsigned)s_checkpoint_mode);
    cfgini_add(&w, "Traffic = %u\r\n", (unsigned)s_traffic_enabled);
    cfgini_add(&w, "SpecialEncounter = %u\r\n", (unsigned)s_special_encounter);
    cfgini_add(&w, "CircuitLaps = %u\r\n", (unsigned)s_circuit_lap_count);
    cfgini_add(&w, "MastersScheduleBase = %u\r\n", (unsigned)s_masters_schedule_base);
    cfgini_add(&w, "P1ScheduleIndex = %u\r\n", (unsigned)s_p1_cup_schedule_index);
    cfgini_add(&w, "P2ScheduleIndex = %u\r\n", (unsigned)s_p2_cup_schedule_index);
    cfgini_add(&w, "P1CupCompletion = %u\r\n", (unsigned)s_p1_cup_completion_bitmask);
    cfgini_add(&w, "P2CupCompletion = %u\r\n", (unsigned)s_p2_cup_completion_bitmask);
    cfgini_add(&w, "P1SelectedCup = %u\r\n", (unsigned)s_p1_selected_cup_id);
    cfgini_add(&w, "MastersEncounterFlags = %u\r\n", (unsigned)s_masters_encounter_flags);
    cfgini_add(&w, "P1MastersUnlock = %u\r\n", (unsigned)s_p1_masters_unlock_bitmask);
    cfgini_add(&w, "P2MastersUnlock = %u\r\n", (unsigned)s_p2_masters_unlock_bitmask);
    cfgini_add(&w, "P2CupLock = %u\r\n\r\n", (unsigned)s_p2_cup_lock_flag);

    cfgini_add(&w, "[Cars]\r\n; Player + 5 AI car indices for the in-progress cup.\r\n");
    cfgini_add(&w, "Player = %d\r\n", g_td5.car_index);
    for (int i = 1; i <= 5; i++)
        cfgini_add(&w, "AI%d = %d\r\n", i, g_td5.ai_car_indices[i]);
    cfgini_add(&w, "\r\n");

    cfgini_add(&w, "[Schedule]\r\n");
    for (int i = 0; i < 0x1E; i++)
        cfgini_add(&w, "S%02d = %u\r\n", i, (unsigned)s_race_schedule[i]);
    cfgini_add(&w, "\r\n[Results]\r\n");
    for (int i = 0; i < 0x1E; i++)
        cfgini_add(&w, "R%02d = %u\r\n", i, (unsigned)s_race_results[i]);
    cfgini_add(&w, "\r\n[SlotState]\r\n");
    for (int i = 0; i < 6; i++)
        cfgini_add(&w, "Slot%d = %u\r\n", i, (unsigned)s_slot_state[i]);

    return cfgini_flush(&w, path);
}

static int cfgini_read_cup(const char *path)
{
    if (!td5_plat_file_exists(path)) return 0;
    int gt = td5_plat_ini_get_int(path, "Cup", "GameType", 0);
    if (gt <= 0) return 0;  /* no valid cup saved */

    char key[8];
    s_selected_game_type   = (uint32_t)gt;
    s_race_within_series   = cfgini_get_u32(path, "Cup", "RaceIndex", 0);
    s_npc_group_index      = cfgini_get_u32(path, "Cup", "NpcGroupIndex", 0);
    s_track_opponent_state = cfgini_get_u32(path, "Cup", "TrackOpponentState", 0);
    s_race_rule_variant    = cfgini_get_u32(path, "Cup", "RaceRuleVariant", 0);
    s_time_trial_enabled   = cfgini_get_u32(path, "Cup", "TimeTrial", 0);
    s_wanted_enabled       = cfgini_get_u32(path, "Cup", "Wanted", 0);
    s_difficulty_tier      = cfgini_get_u32(path, "Cup", "Difficulty", 0);
    s_checkpoint_mode      = cfgini_get_u32(path, "Cup", "CheckpointMode", 0);
    s_traffic_enabled      = cfgini_get_u32(path, "Cup", "Traffic", 0);
    s_special_encounter    = cfgini_get_u32(path, "Cup", "SpecialEncounter", 0);
    s_circuit_lap_count    = cfgini_get_u32(path, "Cup", "CircuitLaps", 0);
    s_masters_schedule_base       = cfgini_get_u32(path, "Cup", "MastersScheduleBase", 0);
    s_p1_cup_schedule_index       = cfgini_get_u32(path, "Cup", "P1ScheduleIndex", 0);
    s_p2_cup_schedule_index       = cfgini_get_u32(path, "Cup", "P2ScheduleIndex", 0);
    s_p1_cup_completion_bitmask   = (uint16_t)cfgini_get_u32(path, "Cup", "P1CupCompletion", 0);
    s_p2_cup_completion_bitmask   = (uint16_t)cfgini_get_u32(path, "Cup", "P2CupCompletion", 0);
    s_p1_selected_cup_id          = (uint8_t)cfgini_get_u32(path, "Cup", "P1SelectedCup", 0);
    s_masters_encounter_flags     = cfgini_get_u32(path, "Cup", "MastersEncounterFlags", 0);
    s_p1_masters_unlock_bitmask   = cfgini_get_u32(path, "Cup", "P1MastersUnlock", 0);
    s_p2_masters_unlock_bitmask   = cfgini_get_u32(path, "Cup", "P2MastersUnlock", 0);
    s_p2_cup_lock_flag            = (uint8_t)cfgini_get_u32(path, "Cup", "P2CupLock", 0);

    /* Car selections -> overlay so td5_save_sync_cup_to_game pushes them
     * into g_td5 (matching the legacy CupData overlay behaviour). */
    s_overlay_car_indices[0] = cfgini_get_i32(path, "Cars", "Player", g_td5.car_index);
    for (int i = 1; i <= 5; i++) {
        snprintf(key, sizeof key, "AI%d", i);
        s_overlay_car_indices[i] = cfgini_get_i32(path, "Cars", key, g_td5.ai_car_indices[i]);
    }
    s_overlay_present = 1;

    for (int i = 0; i < 0x1E; i++) {
        snprintf(key, sizeof key, "S%02d", i);
        s_race_schedule[i] = cfgini_get_u32(path, "Schedule", key, 0);
        snprintf(key, sizeof key, "R%02d", i);
        s_race_results[i] = cfgini_get_u32(path, "Results", key, 0);
    }
    for (int i = 0; i < 6; i++) {
        snprintf(key, sizeof key, "Slot%d", i);
        s_slot_state[i] = cfgini_get_u32(path, "SlotState", key, 0);
    }

    /* The actor snapshot is intentionally not persisted -- clear any stale
     * in-memory copy so the next race starts from a clean grid. */
    memset(s_actor_table, 0, sizeof s_actor_table);
    return 1;
}

void td5_save_delete_cup_data(void)
{
    td5_plat_file_delete(cfgini_cup_path());
    /* Also drop a not-yet-migrated legacy binary so it cannot resurrect. */
    if (td5_plat_file_exists(TD5_CUPDATA_FILENAME))
        td5_plat_file_delete(TD5_CUPDATA_FILENAME);
}

/* ========================================================================
 * Game Options Access
 * ======================================================================== */

TD5_GameOptions *td5_save_get_game_options(void)
{
    return &s_game_options;
}

/* ========================================================================
 * High-score (NPC) table access
 * ======================================================================== */

const uint8_t *td5_save_get_npc_table(void)
{
    return s_npc_group_table;
}

const TD5_NpcGroup *td5_save_get_npc_group(int group_index)
{
    if (group_index < 0 || group_index >= TD5_CONFIG_NPC_GROUPS) return NULL;
    return (const TD5_NpcGroup *)(s_npc_group_table + group_index * TD5_CONFIG_NPC_GROUP_SIZE);
}

/* Returns a mutable pointer to the specified NPC group for name-entry insert.
 * [CONFIRMED @ 0x00413BC0 case 4] original writes directly to g_npcRacerGroupTable. */
TD5_NpcGroup *td5_save_get_npc_group_mutable(int group_index)
{
    if (group_index < 0 || group_index >= TD5_CONFIG_NPC_GROUPS) return NULL;
    return (TD5_NpcGroup *)(s_npc_group_table + group_index * TD5_CONFIG_NPC_GROUP_SIZE);
}

/* -- TD5RE high-score extensions (full names + collisions + air time) -------- */

const TD5_NpcEntryExt *td5_save_get_npc_ext(int group_index, int entry)
{
    if (group_index < 0 || group_index >= TD5_CONFIG_NPC_GROUPS) return NULL;
    if (entry < 0 || entry >= 5) return NULL;
    return &s_npc_ext[group_index][entry];
}

const TD5_NpcEntryExt *td5_save_get_td6_ext(int td6_level, int entry)
{
    if (td6_level < 0 || td6_level >= TD5_MAX_TD6_RECORD_LEVELS) return NULL;
    if (entry < 0 || entry >= 5) return NULL;
    return &s_td6_ext[td6_level][entry];
}

/* Sorted insert into a TD5 NPC group, keeping the parallel extension (full name +
 * collisions + air time) in lockstep. Mirrors the original name-entry insert
 * [CONFIRMED @ 0x00413CB0 case 4] and is shared by the single-player name-entry
 * flow and multiplayer split-screen auto-registration. Persists progress.ini. */
int td5_save_npc_record_insert(int group_index, const char *full_name,
                               int32_t score, int car_id,
                               int32_t avg_speed, int32_t top_speed,
                               int32_t collisions, int32_t air_ticks)
{
    if (group_index < 0 || group_index >= TD5_CONFIG_NPC_GROUPS) return -1;
    if (score == 0) return -1;                 /* DNF / no time = no record */

    TD5_NpcGroup    *grp = (TD5_NpcGroup *)(s_npc_group_table + group_index * TD5_CONFIG_NPC_GROUP_SIZE);
    TD5_NpcEntryExt *ext = s_npc_ext[group_index];
    int type = grp->header & 3;                /* 0/1 = time (lower better), 2 = points (higher better) */

    int ins_pos = 5;
    for (int k = 0; k < 5; k++) {
        if (type < 2) { if (score <= grp->entries[k].score) { ins_pos = k; break; } }
        else          { if (score >= grp->entries[k].score) { ins_pos = k; break; } }
    }
    if (ins_pos >= 5) return -1;               /* didn't beat any of the 5 */

    /* Shift lower entries (and their extensions) down one slot, then write. */
    for (int k = 3; k >= ins_pos; k--) {
        grp->entries[k + 1] = grp->entries[k];
        ext[k + 1]          = ext[k];
    }
    TD5_NpcEntry *en = &grp->entries[ins_pos];
    memset(en, 0, sizeof(*en));
    if (full_name) strncpy(en->name, full_name, sizeof(en->name) - 1);   /* 15-char legacy field */
    en->score     = score;
    en->car_id    = car_id;
    en->avg_speed = avg_speed;
    en->top_speed = top_speed;

    memset(&ext[ins_pos], 0, sizeof(ext[ins_pos]));
    if (full_name) strncpy(ext[ins_pos].full_name, full_name, sizeof(ext[ins_pos].full_name) - 1);
    ext[ins_pos].collisions = collisions;
    ext[ins_pos].air_ticks  = air_ticks;

    cfgini_write_progress();                   /* persist ext + entries to progress.ini */
    TD5_LOG_I(LOG_TAG, "NPC record inserted: group=%d rank=%d name='%s' score=%d coll=%d air=%d",
              group_index, ins_pos, en->name, (int)score, (int)collisions, (int)air_ticks);
    return ins_pos;
}

int td5_save_get_speed_units(void)
{
    return (int)s_speed_units;
}

int td5_save_get_circuit_lap_count(void)
{
    return (int)s_circuit_lap_count;
}

/* [CONFIRMED @ 0x00427156 ScreenLocalizationInit] Original seeds
 * gConfiguredDisplayModeOrdinal from gSelectedDisplayModeOrdinal (loaded from
 * config.td5 by LoadPackedConfigTd5 @ 0x0040FB60). The port persists this
 * field as s_display_mode at byte 0xBD; expose it so the frontend can seed
 * its runtime selector at boot instead of recomputing it from the live
 * window dimensions every time DisplayOptions opens. */
int td5_save_get_display_mode(void)
{
    return (int)s_display_mode;
}

void td5_save_set_display_mode(int v)
{
    s_display_mode = v;
}

/* ========================================================================
 * Unlock System
 *
 * Car lock table: 37 bytes, 0 = unlocked, 1 = locked.
 * Track lock table: 26 bytes, 0 = locked, 1 = unlocked.
 * ======================================================================== */

int td5_save_is_car_unlocked(int car_index)
{
    if (car_index < 0 || car_index >= TD5_CONFIG_NUM_CARS) {
        return 0;
    }
    if (s_all_cars_unlocked) {
        return 1;
    }
    return (s_car_locks[car_index] == 0) ? 1 : 0;
}

int td5_save_is_track_unlocked(int track_index)
{
    if (track_index < 0 || track_index >= TD5_CONFIG_NUM_TRACKS) {
        return 0;
    }
    return (s_track_locks[track_index] != 0) ? 1 : 0;
}

void td5_save_unlock_car(int car_index)
{
    if (car_index >= 0 && car_index < TD5_CONFIG_NUM_CARS) {
        s_car_locks[car_index] = 0;
        if (s_max_unlocked_car < (uint32_t)(car_index + 1)) {
            s_max_unlocked_car = (uint32_t)(car_index + 1);
        }
    }
}

void td5_save_unlock_track(int track_index)
{
    if (track_index >= 0 && track_index < TD5_CONFIG_NUM_TRACKS) {
        s_track_locks[track_index] = 1;
    }
}

int td5_save_get_max_unlocked_car(void)
{
    return (int)s_max_unlocked_car;
}

int td5_save_get_all_cars_unlocked(void)
{
    return s_all_cars_unlocked ? 1 : 0;
}

int td5_save_get_cup_tier(void)
{
    return (int)(s_cup_tier & 0x07);
}

void td5_save_get_car_lock_table(uint8_t *out_car_locks, int count)
{
    int i;
    if (!out_car_locks) return;
    if (count > TD5_CONFIG_NUM_CARS) count = TD5_CONFIG_NUM_CARS;
    for (i = 0; i < count; i++) {
        /* s_car_locks: 0=unlocked, 1=locked -- pass through directly */
        if (s_all_cars_unlocked) {
            out_car_locks[i] = 0; /* unlocked */
        } else {
            out_car_locks[i] = s_car_locks[i];
        }
    }
}

void td5_save_get_track_lock_table(uint8_t *out_track_locks, int count)
{
    int i;
    if (!out_track_locks) return;
    if (count > TD5_CONFIG_NUM_TRACKS) count = TD5_CONFIG_NUM_TRACKS;
    for (i = 0; i < count; i++) {
        /* s_track_locks: 1=unlocked, 0=locked -- invert for caller */
        out_track_locks[i] = (s_track_locks[i] != 0) ? 0 : 1;
    }
}

/* ========================================================================
 * Cup Unlock Tables
 *
 * Derived from RE analysis of the original binary's progression system.
 * The original game has 6 cup types (game types 1-6), each unlocking
 * specific cars and tracks upon completion.
 *
 * Cup tier bits (s_cup_tier, 3 bits):
 *   bit 0: Championship completed
 *   bit 1: Challenge completed
 *   bit 2: Pitbull completed
 *
 * Original progression (from Ghidra analysis of 0x410CA0 and 0x423A80):
 *   - Fresh save: 21 cars visible (indices 0-20), 2 locked (21-22).
 *     Tracks 0-19 unlocked (race tracks), 20-25 locked (cup tracks).
 *   - Championship (type 1) won: unlocks car 23 (Dodge Viper GTS-R),
 *     car 24 (McLaren F1 GTR), track 20 (Cup track 1).
 *   - Era (type 2) won: unlocks car 25 (Lister Storm).
 *   - Challenge (type 3) won: unlocks car 26 (Panoz Esperante GTR-1),
 *     car 27 (Mercedes CLK-GTR), tracks 21-22.
 *   - Pitbull (type 4) won: unlocks car 28 (Porsche 911 GT1),
 *     car 29 (Toyota GT-One), tracks 23-24.
 *   - Masters (type 5) won: unlocks car 30 (Nissan R390 GT1),
 *     car 31 (BMW V12 LMR), track 25.
 *   - Cop Chase (type 8) and others: no unlock progression.
 *
 * The initial lock state for a fresh game:
 *   Cars 0-20: unlocked (standard roster)
 *   Cars 21-22: locked (visible but locked: SUPER7, R390)
 *   Cars 23-36: locked (hidden in normal mode, cop-chase/cup only)
 *   Tracks 0-19: unlocked (race tracks)
 *   Tracks 20-25: locked (cup tracks)
 * ======================================================================== */

int td5_save_apply_cup_unlocks_ex(int game_type, int *cars_out, int *tracks_out)
{
    int car_count = 0, track_count = 0;

    /* Placement validation gate.
     * [CONFIRMED @ 0x421DA0 AwardCupCompletionUnlocks]:
     *   Original checks gRaceSlotStateTable.slot[0].companion_state_2 == 1 (finished, placed)
     *   AND g_actorRuntimeState.slot._899_1_ == 0 (not disqualified).
     * Port equivalent: slot 0 must be finished and not in a DNQ state (state != 3).
     * game_type must also be in cup range 1-6; types 7/8/9/0 use per-track unlock
     * via AwardCupCompletionUnlocks which maps by schedule index, not game_type.
     * For game_type == -1 (refresh-only), skip placement check and fall through. */
    if (game_type != -1) {
        if (game_type < 1 || game_type > 6) {
            TD5_LOG_D(LOG_TAG, "apply_cup_unlocks: game_type=%d not in cup range, skip", game_type);
            if (cars_out)   *cars_out   = 0;
            if (tracks_out) *tracks_out = 0;
            return 0;
        }
        if (!td5_game_slot_is_finished(0)) {
            TD5_LOG_W(LOG_TAG, "apply_cup_unlocks: slot 0 not finished, skip unlocks");
            if (cars_out)   *cars_out   = 0;
            if (tracks_out) *tracks_out = 0;
            return 0;
        }
        if (td5_game_get_slot_state(0) == 3) {
            TD5_LOG_W(LOG_TAG, "apply_cup_unlocks: slot 0 disqualified (state=3), skip unlocks");
            if (cars_out)   *cars_out   = 0;
            if (tracks_out) *tracks_out = 0;
            return 0;
        }
    }

    /* Apply unlocks based on the cup that was just won */
    switch (game_type) {
    case 1: /* Championship */
        if (!(s_cup_tier & 0x01)) {
            s_cup_tier |= 0x01;
        }
        /* Unlock cars 23, 24 (Dodge Viper GTS-R, McLaren F1 GTR) */
        if (s_car_locks[23] != 0) { s_car_locks[23] = 0; car_count++; }
        if (s_car_locks[24] != 0) { s_car_locks[24] = 0; car_count++; }
        /* Unlock cup track 20 */
        if (s_track_locks[20] == 0) { s_track_locks[20] = 1; track_count++; }
        break;

    case 2: /* Era */
        /* Unlock car 25 (Lister Storm) */
        if (s_car_locks[25] != 0) { s_car_locks[25] = 0; car_count++; }
        break;

    case 3: /* Challenge */
        if (!(s_cup_tier & 0x02)) {
            s_cup_tier |= 0x02;
        }
        /* Unlock cars 26, 27 (Panoz Esperante GTR-1, Mercedes CLK-GTR) */
        if (s_car_locks[26] != 0) { s_car_locks[26] = 0; car_count++; }
        if (s_car_locks[27] != 0) { s_car_locks[27] = 0; car_count++; }
        /* Unlock cup tracks 21, 22 */
        if (s_track_locks[21] == 0) { s_track_locks[21] = 1; track_count++; }
        if (s_track_locks[22] == 0) { s_track_locks[22] = 1; track_count++; }
        break;

    case 4: /* Pitbull */
        if (!(s_cup_tier & 0x04)) {
            s_cup_tier |= 0x04;
        }
        /* Unlock cars 28, 29 (Porsche 911 GT1, Toyota GT-One) */
        if (s_car_locks[28] != 0) { s_car_locks[28] = 0; car_count++; }
        if (s_car_locks[29] != 0) { s_car_locks[29] = 0; car_count++; }
        /* Unlock cup tracks 23, 24 */
        if (s_track_locks[23] == 0) { s_track_locks[23] = 1; track_count++; }
        if (s_track_locks[24] == 0) { s_track_locks[24] = 1; track_count++; }
        break;

    case 5: /* Masters */
        /* Unlock cars 30, 31 (Nissan R390 GT1, BMW V12 LMR) */
        if (s_car_locks[30] != 0) { s_car_locks[30] = 0; car_count++; }
        if (s_car_locks[31] != 0) { s_car_locks[31] = 0; car_count++; }
        /* Unlock cup track 25 */
        if (s_track_locks[25] == 0) { s_track_locks[25] = 1; track_count++; }
        break;

    case 6: /* Drag */
        /* Unlock car 32 (unlocks access to hidden car index 32) */
        if (s_car_locks[32] != 0) { s_car_locks[32] = 0; car_count++; }
        break;

    default:
        /* game_type == -1: just refresh max_unlocked_car from lock table */
        break;
    }

    /* Recompute max_unlocked_car from the lock table */
    {
        int i;
        uint32_t max_car = 0;
        for (i = 0; i < TD5_CONFIG_NUM_CARS; i++) {
            if (s_car_locks[i] == 0) {
                if ((uint32_t)(i + 1) > max_car) {
                    max_car = (uint32_t)(i + 1);
                }
            }
        }
        s_max_unlocked_car = max_car;
    }

    TD5_LOG_I(LOG_TAG, "apply_cup_unlocks: game_type=%d tier=0x%02X max_car=%u cars=%d tracks=%d",
              game_type, (unsigned)s_cup_tier, (unsigned)s_max_unlocked_car, car_count, track_count);

    if (cars_out)   *cars_out   = car_count;
    if (tracks_out) *tracks_out = track_count;
    return car_count + track_count;
}

int td5_save_apply_cup_unlocks(int game_type)
{
    return td5_save_apply_cup_unlocks_ex(game_type, NULL, NULL);
}

/* ========================================================================
 * td5_save_test_cup_roundtrip  (port-only divergence self-test)
 *
 * Exercises the extended CupData.td5 format end-to-end: seeds known car
 * indices, serializes + writes, wipes runtime state, reads + deserializes,
 * and asserts that g_td5.car_index/ai_car_indices restore correctly.
 * Returns 1 on pass, 0 on fail.
 *
 * Side effects: writes/deletes "test_cup_roundtrip.td5" in cwd, mutates
 * g_td5.car_index, g_td5.ai_car_indices[6], and the entire
 * s_actor_table. Intended to run before any race state exists.
 * ======================================================================== */

int td5_save_test_cup_roundtrip(void)
{
    /* Absolute path: the Win32 profile API resolves a *relative* INI name
     * against C:\Windows, so cfgini_read_cup would never find a cwd-relative
     * test file. Resolve next to the exe like the real cup INI. */
    char test_path[TD5_CFGINI_PATH_MAX];
    td5_plat_ini_resolve_path("test_cup_roundtrip.ini", test_path, sizeof test_path);
    int pass = 1;

    /* Seed g_td5 with a deterministic mix and pre-populate the actor
     * table's pointer slots with sentinel "dangling" addresses so we can
     * verify the post-load NULL scrub. */
    const int      saved_car        = g_td5.car_index;
    const uint32_t saved_game_type  = s_selected_game_type;
    const int saved_ai[6]    = {
        g_td5.ai_car_indices[0], g_td5.ai_car_indices[1],
        g_td5.ai_car_indices[2], g_td5.ai_car_indices[3],
        g_td5.ai_car_indices[4], g_td5.ai_car_indices[5],
    };

    /* cfgini_write_cup only emits a continuable cup when GameType > 0. */
    s_selected_game_type        = 1;   /* Championship */
    g_td5.car_index             = 5;
    g_td5.ai_car_indices[0]     = 0;   /* slot 0 mirror, not persisted */
    g_td5.ai_car_indices[1]     = 11;
    g_td5.ai_car_indices[2]     = 22;
    g_td5.ai_car_indices[3]     = 7;
    g_td5.ai_car_indices[4]     = 14;
    g_td5.ai_car_indices[5]     = 3;

    memset(s_actor_table, 0, sizeof(s_actor_table));
    for (int slot = 0; slot < 6; slot++) {
        uint8_t *a = (uint8_t *)s_actor_table + (size_t)slot * 0x388;
        write_le32(a + 0x1B0, 0xDEADBEEF);
        write_le32(a + 0x1B8, 0xCAFEBABE);
        write_le32(a + 0x1BC, 0xF00DD00D);
    }

    if (!td5_save_write_cup_data(test_path)) {
        TD5_LOG_E(LOG_TAG, "test_cup_roundtrip: write failed");
        pass = 0;
        goto restore;
    }

    /* Wipe runtime state so any retained residue would be visible. */
    g_td5.car_index = -1;
    for (int i = 0; i < 6; i++) g_td5.ai_car_indices[i] = -1;
    memset(s_actor_table, 0, sizeof(s_actor_table));
    s_overlay_present = 0;
    for (int i = 0; i < TD5_CUPDATA_OVERLAY_NUM_SLOTS; i++) {
        s_overlay_car_indices[i] = -2;
    }

    if (!td5_save_load_cup_data(test_path)) {
        TD5_LOG_E(LOG_TAG, "test_cup_roundtrip: load failed");
        pass = 0;
        goto restore;
    }

    if (!s_overlay_present) {
        TD5_LOG_E(LOG_TAG, "test_cup_roundtrip: overlay not detected after load");
        pass = 0;
    }

    /* Push overlay -> g_td5 (sync_cup_to_game would also do this). */
    (void)td5_save_sync_cup_to_game(NULL);

    const int want_car   = 5;
    const int want_ai[6] = { 5, 11, 22, 7, 14, 3 }; /* [0]=player mirror */
    if (g_td5.car_index != want_car) {
        TD5_LOG_E(LOG_TAG, "test_cup_roundtrip: car_index got=%d want=%d",
                  g_td5.car_index, want_car);
        pass = 0;
    }
    /* Slots 1..5 are the only ones the overlay persists; slot 0's
     * ai_car_indices entry is unused by the original flow and is left
     * untouched on load (current sync_cup_to_game does not write it). */
    for (int i = 1; i <= 5; i++) {
        if (g_td5.ai_car_indices[i] != want_ai[i]) {
            TD5_LOG_E(LOG_TAG,
                      "test_cup_roundtrip: ai_car_indices[%d] got=%d want=%d",
                      i, g_td5.ai_car_indices[i], want_ai[i]);
            pass = 0;
        }
    }

    /* Verify defensive pointer-slot scrub for slots 0..5. */
    for (int slot = 0; slot < 6; slot++) {
        const uint8_t *a = (const uint8_t *)s_actor_table +
                           (size_t)slot * 0x388;
        uint32_t p1B0 = read_le32(a + 0x1B0);
        uint32_t p1B8 = read_le32(a + 0x1B8);
        uint32_t p1BC = read_le32(a + 0x1BC);
        if (p1B0 != 0 || p1B8 != 0 || p1BC != 0) {
            TD5_LOG_E(LOG_TAG,
                      "test_cup_roundtrip: slot=%d ptr scrub failed "
                      "p1B0=0x%08X p1B8=0x%08X p1BC=0x%08X",
                      slot, p1B0, p1B8, p1BC);
            pass = 0;
        }
    }

    /* (Subtest 2, the legacy 12966-byte binary CupData.td5 compatibility
     * check, was removed when the binary cup format was retired in favour of
     * td5re_cup.ini. Legacy import is exercised by the migration path in
     * td5_save_load_cup_data instead.) */

    TD5_LOG_I(LOG_TAG, "test_cup_roundtrip: %s", pass ? "PASS" : "FAIL");

    /* Cleanup: leave the extended-format file in place if FAIL (for
     * inspection), remove it on PASS so the next run starts clean. */
    if (pass) {
        remove(test_path);
    }

restore:
    g_td5.car_index = saved_car;
    s_selected_game_type = saved_game_type;
    for (int i = 0; i < 6; i++) g_td5.ai_car_indices[i] = saved_ai[i];
    memset(s_actor_table, 0, sizeof(s_actor_table));

    return pass;
}
