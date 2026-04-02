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
#include <string.h>

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
    /* 0x328C */ uint32_t cup_sub_state_b;                                   /* VERIFIED: 0x48F310, addr calc 0x493E38-0x490BAC=0x328C */
    /* 0x3290 */ uint32_t cup_sub_state_a;                                   /* VERIFIED: 0x48F314, addr calc 0x493E3C-0x490BAC=0x3290 */
    /* 0x3294 */ uint16_t cup_sub_word_a;                                    /* VERIFIED: 0x48F318, addr calc 0x493E40-0x490BAC=0x3294 (2 bytes, not 4) */
    /* 0x3296 */ uint8_t  cup_sub_byte_a;                                    /* VERIFIED: 0x48F31A, addr calc 0x493E42-0x490BAC=0x3296 */
    /* 0x3297 */ uint32_t masters_encounter_flags;                           /* VERIFIED: 0x48F324, addr calc 0x493E43-0x490BAC=0x3297 */
    /* 0x329B */ uint32_t cup_sub_state_c;                                   /* VERIFIED: 0x48F328, addr calc 0x493E47-0x490BAC=0x329B */
    /* 0x329F */ uint32_t cup_sub_state_d;                                   /* VERIFIED: 0x48F32C, addr calc 0x493E4B-0x490BAC=0x329F */
    /* 0x32A3 */ uint16_t cup_sub_word_b;                                    /* VERIFIED: 0x48F330, addr calc 0x493E4F-0x490BAC=0x32A3 */
    /* 0x32A5 */ uint8_t  cup_sub_byte_b;                                    /* VERIFIED: 0x48F332, addr calc 0x493E51-0x490BAC=0x32A5 */
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

/** CupData.td5 snapshot buffer (12966 bytes, mirrors 0x490BAC). */
static uint8_t s_cup_buf[TD5_CUPDATA_FILE_SIZE];

/** CupData snapshot byte count (mirrors DAT_00494BBC). */
static uint32_t s_cup_buf_size;

/* -- Globals that feed into Config.td5 -- */
static TD5_GameOptions s_game_options;                        /* 0x466000 */
static uint32_t s_p1_device_index;                            /* 0x497A58 */
static uint32_t s_p2_device_index;                            /* 0x465FF4 */
static int32_t  s_ff_config[4];                               /* 0x464054, 0x46405C, 0x464058, 0x464060 */
static uint32_t s_controller_bindings[18];                    /* 0x463FC4 */
static uint32_t s_p1_device_desc[8];                          /* 0x465660 (write) / 0x4656A0 (read) */
static uint32_t s_p2_device_desc[8];                          /* 0x465680 (write) / 0x4656C0 (read) */
static uint32_t s_p1_device_desc_backup[8];                   /* 0x4656A0 (read target) */
static uint32_t s_p2_device_desc_backup[8];                   /* 0x4656C0 (read target) */
static uint32_t s_sound_mode;                                 /* 0x465FE8 */
static uint32_t s_sfx_volume;                                 /* 0x465FEC */
static uint32_t s_music_volume;                               /* 0x465FF0 */
static int32_t  s_display_mode;                               /* 0x466020 */
static int32_t  s_fog_enabled;                                /* 0x466024 */
static int32_t  s_speed_units;                                /* 0x466028 */
static int32_t  s_camera_damping;                             /* 0x46602C */
static uint32_t s_p1_custom_bindings[0x62];                   /* 0x4978C0 */
static uint32_t s_p2_custom_bindings[0x62];                   /* 0x497330 */
static uint32_t s_split_screen_mode;                          /* 0x497A5C */
static uint32_t s_catchup_assist;                             /* 0x465FF8 */
static uint8_t  s_camera_byte_a;                              /* 0x482F48 */
static uint8_t  s_camera_byte_b;                              /* 0x482F49 */
static uint32_t s_music_track;                                /* 0x466840 */
static uint8_t  s_npc_group_table[TD5_CONFIG_NPC_TABLE_SIZE]; /* 0x4643B8 */
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
static uint32_t s_cup_sub_state_a;                            /* 0x48F314 */
static uint32_t s_cup_sub_state_b;                            /* 0x48F310 */
static uint8_t  s_cup_sub_byte_a;                             /* 0x48F31A */
static uint16_t s_cup_sub_word_a;                             /* 0x48F318 */
static uint32_t s_cup_sub_state_c;                            /* 0x48F328 */
static uint32_t s_masters_encounter_flags;                    /* 0x48F324 */
static uint32_t s_cup_sub_state_d;                            /* 0x48F32C */
static uint16_t s_cup_sub_word_b;                             /* 0x48F330 */
static uint8_t  s_cup_sub_byte_b;                             /* 0x48F332 */

/* Cross-references restored from within the schedule region. */
static uint32_t s_cup_progress_marker;                        /* 0x48F364 */
static uint32_t s_cup_cross_ref_1;                            /* 0x48F368 */
static uint32_t s_cup_cross_ref_2;                            /* 0x48F370 */
static uint32_t s_cup_cross_ref_3;                            /* 0x48F378 */

/* ========================================================================
 * Initialization / Shutdown
 * ======================================================================== */

int td5_save_init(void)
{
    memset(s_config_buf, 0, sizeof(s_config_buf));
    memset(s_cup_buf, 0, sizeof(s_cup_buf));
    s_cup_buf_size = 0;
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

static void config_serialize_to_buffer(void)
{
    TD5_ConfigBuffer *buf = (TD5_ConfigBuffer *)s_config_buf;

    /* Game options: 7 dwords at offset 0x04. */
    memcpy(buf->game_options, &s_game_options, sizeof(buf->game_options));

    /* Controller device indices. */
    buf->p1_device_index = (uint8_t)s_p1_device_index;
    buf->p2_device_index = (uint8_t)s_p2_device_index;

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
    const char *filepath = path ? path : TD5_CONFIG_FILENAME;
    TD5_ConfigBuffer *buf = (TD5_ConfigBuffer *)s_config_buf;
    int ok;

    /* Step 1: Serialize all fields into the buffer. */
    config_serialize_to_buffer();

    /* Step 2: Set CRC placeholder, compute CRC-32 over entire buffer. */
    buf->crc32 = TD5_CRC_PLACEHOLDER;
    uint32_t crc = td5_save_crc32(s_config_buf, TD5_CONFIG_FILE_SIZE);
    buf->crc32 = crc;

    /* Step 3: XOR-encrypt the entire buffer in-place. */
    td5_save_xor_encrypt(s_config_buf, TD5_CONFIG_FILE_SIZE, TD5_CONFIG_XOR_KEY);

    /* Step 4: Write to file. */
    TD5_File *f = td5_plat_file_open(filepath, "wb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Write config failed: path=%s crc=0x%08X size=%u",
                  filepath, (unsigned int)crc, (unsigned int)TD5_CONFIG_FILE_SIZE);
        return 0;
    }
    size_t written = td5_plat_file_write(f, s_config_buf, TD5_CONFIG_FILE_SIZE);
    td5_plat_file_close(f);

    ok = (written == TD5_CONFIG_FILE_SIZE) ? 1 : 0;
    TD5_LOG_I(LOG_TAG, "Write config %s: path=%s crc=0x%08X size=%u",
              ok ? "ok" : "failed", filepath, (unsigned int)crc, (unsigned int)written);
    return ok;
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

    /* Controller device indices. */
    s_p1_device_index = buf->p1_device_index;
    s_p2_device_index = buf->p2_device_index;

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
 * ======================================================================== */

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
     *   0x493E3C - 0x490BAC = 0x3290  (cup_sub_state_a = 0x48F314)
     *   0x493E38 - 0x490BAC = 0x328C  (cup_sub_state_b = 0x48F310)  -- NOTE: 0x328C not 0x3294
     *   0x493E42 - 0x490BAC = 0x3296  (cup_sub_byte_a  = 0x48F31A)
     *   0x493E40 - 0x490BAC = 0x3294  (cup_sub_word_a  = 0x48F318)  -- NOTE: 0x3294 not 0x3297
     *   0x493E47 - 0x490BAC = 0x329B  (cup_sub_state_c = 0x48F328)
     *   0x493E43 - 0x490BAC = 0x3297  (masters_encounter_flags = 0x48F324)
     *   0x493E4B - 0x490BAC = 0x329F  (sub_word_b region = 0x48F32C)
     *   0x493E4F - 0x490BAC = 0x32A3  (cup_sub_word_b = 0x48F330)
     *   0x493E51 - 0x490BAC = 0x32A5  (cup_sub_byte_b = 0x48F332)
     */
    write_le32(buf + 0x3288, s_masters_schedule_base);
    write_le32(buf + 0x328C, s_cup_sub_state_b);
    write_le32(buf + 0x3290, s_cup_sub_state_a);
    write_le16(buf + 0x3294, s_cup_sub_word_a);
    buf[0x3296] = s_cup_sub_byte_a;
    write_le32(buf + 0x3297, s_masters_encounter_flags);
    write_le32(buf + 0x329B, s_cup_sub_state_c);
    write_le32(buf + 0x329F, s_cup_sub_state_d); /* VERIFIED: 0x48F32C, addr calc 0x493E4B-0x490BAC=0x329F */
    write_le16(buf + 0x32A3, s_cup_sub_word_b);
    buf[0x32A5] = s_cup_sub_byte_b;

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
    write_le32(buf + 0x0C, TD5_CRC_PLACEHOLDER);
    uint32_t crc = td5_save_crc32(buf, TD5_CUPDATA_FILE_SIZE);
    write_le32(buf + 0x0C, crc);

    /* Store snapshot size. */
    s_cup_buf_size = TD5_CUPDATA_FILE_SIZE;
}

/* ========================================================================
 * RestoreRaceStatusSnapshot  (original VA 0x4112C0)
 *
 * Validates CRC-32 embedded at offset +0x0C, then restores all cup globals
 * from the snapshot buffer. Returns 1 on success, 0 on failure.
 * ======================================================================== */

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
    s_cup_sub_state_b         = read_le32(buf + 0x328C);
    s_cup_sub_state_a         = read_le32(buf + 0x3290);
    s_cup_sub_word_a          = read_le16(buf + 0x3294);
    s_cup_sub_byte_a          = buf[0x3296];
    s_masters_encounter_flags = read_le32(buf + 0x3297);
    s_cup_sub_state_c         = read_le32(buf + 0x329B);
    s_cup_sub_state_d         = read_le32(buf + 0x329F); /* VERIFIED: 0x48F32C */
    s_cup_sub_word_b          = read_le16(buf + 0x32A3);
    s_cup_sub_byte_b          = buf[0x32A5];

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
    const char *filepath = path ? path : TD5_CUPDATA_FILENAME;
    int ok;

    /* Step 1: Serialize the cup state into the snapshot buffer. */
    cup_serialize_to_buffer();

    if (s_cup_buf_size == 0) {
        TD5_LOG_W(LOG_TAG, "Write cup data failed: path=%s reason=empty", filepath);
        return 0;
    }

    /* Step 2: Make an encrypted copy (original uses stack alloca). */
    uint8_t enc_buf[TD5_CUPDATA_FILE_SIZE];
    memcpy(enc_buf, s_cup_buf, s_cup_buf_size);
    td5_save_xor_encrypt(enc_buf, s_cup_buf_size, TD5_CUPDATA_XOR_KEY);

    /* Step 3: Write to file. */
    TD5_File *f = td5_plat_file_open(filepath, "wb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "Write cup data failed: path=%s reason=open", filepath);
        return 0;
    }
    size_t written = td5_plat_file_write(f, enc_buf, s_cup_buf_size);
    td5_plat_file_close(f);

    ok = (written == s_cup_buf_size) ? 1 : 0;
    TD5_LOG_I(LOG_TAG, "Write cup data %s: path=%s size=%u",
              ok ? "ok" : "failed", filepath, (unsigned int)written);
    return ok;
}

/* ========================================================================
 * LoadContinueCupData  (original VA 0x411590)
 *
 * Reads CupData.td5, XOR-decrypts into the snapshot buffer, then calls
 * RestoreRaceStatusSnapshot to validate CRC and restore globals.
 * Returns 1 on success, 0 on failure.
 * ======================================================================== */

int td5_save_load_cup_data(const char *path)
{
    const char *filepath = path ? path : TD5_CUPDATA_FILENAME;
    int ok;

    /* Step 1: Open and read the file into a temporary stack buffer. */
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

    /* Step 2: XOR-decrypt into the snapshot buffer. */
    for (size_t i = 0; i < bytes_read; i++) {
        s_cup_buf[i] = read_buf[i];
    }
    td5_save_xor_decrypt(s_cup_buf, bytes_read, TD5_CUPDATA_XOR_KEY);

    /* Step 3: Store byte count. */
    s_cup_buf_size = (uint32_t)bytes_read;

    /* Step 4: Validate CRC and restore globals. */
    ok = cup_deserialize_from_buffer();
    TD5_LOG_I(LOG_TAG, "Load cup data %s: path=%s size=%u",
              ok ? "ok" : "failed", filepath, (unsigned int)bytes_read);
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
 * ======================================================================== */

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
 * Game Options Access
 * ======================================================================== */

TD5_GameOptions *td5_save_get_game_options(void)
{
    return &s_game_options;
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
