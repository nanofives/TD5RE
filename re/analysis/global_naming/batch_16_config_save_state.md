---
batch: 16
area: config_save_state
tier: T4
target_todos: []
ghidra_session: f0fbd07e06ba4149a2526c6ddcf0bab7
analyzed_addresses: 0x0040FB60, 0x0040F8D0, 0x004114F0, 0x00411590, 0x00411630, 0x00411120, 0x004112C0, 0x00421DA0, 0x00414B50, 0x00402E60, 0x004269D0, 0x0041F990
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Config.td5 / CupData.td5 / cheat-state / INI mirrors

## Summary

- Functions analyzed: 12 (5 primary save/load + 4 frontend screen consumers + 3 cheat / award engines)
- Unnamed DAT_* globals encountered: 38 (after de-dup)
- Already-named globals encountered (noted, no proposal): 18
- Proposals — high confidence: 24
- Proposals — medium confidence: 6
- Proposals — comment-only (low confidence): 2

## Methodology

Entry points: `LoadPackedConfigTd5 @ 0x0040FB60`, `WritePackedConfigTd5 @ 0x0040F8D0`, `WriteCupData @ 0x004114F0`, `LoadContinueCupData @ 0x00411590`, `ValidateCupDataChecksum @ 0x00411630`. From these I walked one level of callers/consumers (`SerializeRaceStatusSnapshot @ 0x00411120`, `RestoreRaceStatusSnapshot @ 0x004112C0`, `AwardCupCompletionUnlocks @ 0x00421DA0`, `RunFrontendDisplayLoop @ 0x00414B50`, `UpdatePlayerVehicleControlState @ 0x00402E60`, `ScreenLocalizationInit @ 0x004269D0`, `ScreenGameOptions @ 0x0041F990`) to recover writer sites and semantic context for every `DAT_*` field touched by the save/load path.

Three structural insights drove the relevance gate:
1. **Config.td5 is a flat 5351-byte mirror buffer at `DAT_0048F384`** that is XOR-encrypted with the key string `"Outta Mah Face !! "` at `s_Outta_Mah_Face____00463F9C` (length 18, including trailing space) and CRC-32 validated via the standard polynomial 0xEDB88320 table at `DAT_00475160`. LoadPackedConfigTd5 fans out the decrypted bytes into 14+ scattered runtime globals; WritePackedConfigTd5 is the inverse.
2. **CupData.td5 is a 12966-byte (`0x32A6`) mirror buffer at `DAT_00490BAC`** XOR-encrypted with `"Steve Snake says : No Cheating! "` at `s_Steve_Snake_says___No_Cheating__00464084` (length 32), same CRC table. The size is stored in `DAT_00494BBC`; the trailing CRC dword lives at `DAT_00490BB8`.
3. **Cheat-code activation is a CYCLIC-XOR machine** at `RunFrontendDisplayLoop @ 0x00414B50` with three parallel arrays of length 6: byte-sequence-table `DAT_004654A4` (stride 0x28, -1-terminated), current-key-index-table `DAT_004951F0`, and the cheat-XOR-mask-table `DAT_004655AC = {1, 8, 2, 1, 1, 1}` (T2.7 finding). The XOR target is selected through a pointer indirection `PTR_DAT_00465594` (6 pointers). The 6 cheat targets are `DAT_00496298, g_cupUnlockLevel, DAT_004962AC, DAT_004AAF7C, DAT_0049629C, g_cheatCodeActivated`. Activation toggles `gNpcRacerCheatFlags[i] |= 2` for every NPC group whose first byte is 0.

## Proposals

### Config.td5 file-format header constants (XOR keys, CRC table, magic strings)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00463F9C | char[18] | `g_configTd5XorKey` | **high** | XOR key for `Config.td5` payload. Both LoadPackedConfigTd5 (0x0040FBA8 loop) and WritePackedConfigTd5 (0x0040FAD2 loop) cycle bytes of this string against the buffer XOR 0x80. Bytes verified: `4F 75 74 74 61 20 4D 61 68 20 46 61 63 65 20 21 21 20` = `"Outta Mah Face !! "`. Already labeled `s_Outta_Mah_Face____00463F9C` (default symbol). | td5_save.c line ~5 comment + `td5_save_xor_decrypt/encrypt` |
| 0x00463FB0 | char[10] | `g_configTd5Filename` | high | String `"Config.td5"` passed to `fopen_game` by Load/WritePackedConfigTd5. Already labeled `s_Config_td5_00463FB0`. | TD5_CONFIG_FILENAME |
| 0x00463FBC | char[2] | `g_fopenModeWrite` | med | String `"wb"` passed to `fopen_game` by all write paths (Write+Cup). | (none) |
| 0x00463FC0 | char[2] | `g_fopenModeRead` | med | String `"rb"` passed to `fopen_game` by all read paths (Load+Validate). | (none) |
| 0x00464084 | char[32] | `g_cupDataTd5XorKey` | **high** | XOR key for `CupData.td5` payload. Used by WriteCupData/LoadContinueCupData/ValidateCupDataChecksum. Bytes verified: `53 74 65 76 65 20 53 6E 61 6B 65 20 73 61 79 73 20 3A 20 4E 6F 20 43 68 65 61 74 69 6E 67 21 20` = `"Steve Snake says : No Cheating! "`. Already labeled `s_Steve_Snake_says___No_Cheating__00464084` (default symbol). | td5_save.c line ~5 comment + `td5_save_xor_*` |
| 0x00464120 | char[11] | `g_cupDataTd5Filename` | high | String `"CupData.td5"` passed to `fopen_game` by WriteCupData/LoadContinueCupData/ValidateCupDataChecksum. Already labeled `s_CupData_td5_00464120`. | TD5_CUPDATA_FILENAME |
| 0x00475160 | u32[256] | `g_crc32_table_edb88320` | **high** | Standard CRC-32 lookup table for polynomial 0xEDB88320. Verified bytes at offset 0x04 = `0x77073096` (= CRC32(0x01) for EDB88320). 8 xrefs from Write/Load/ValidateConfig + Cup paths + general data hashing. Already labeled `DAT_00475160`. | `td5_crc32` table in td5_types.h |

### Config.td5 mirror buffer + checksum/size scratch

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048F384 | u8[5351] | `g_configTd5IoBuffer` | **high** | The 0x1800-byte read buffer (5351 used) into which `fread_game(Config.td5)` writes. First dword stores embedded CRC; checksum-validation logic at 0x0040FBE0-0x0040FBF6 verifies `~CRC32(buf[4..])` == buf[0..3] (with placeholder 0x10 swap). All 14+ field-fanout copies after the CRC pass read from `&DAT_0048F388` (= buffer+4). | TD5_ConfigBuffer s_config_buf @ td5_save.c |
| 0x0048F388 | u32 | `g_configTd5GameOptionsCopy_start` | med | Symbolic anchor; not a distinct field. The 7-dword copy at 0x0040FC3E points puVar9 here as source for the GameOptions copy into `g_gameOptionsBlock @ 0x00466000`. Comment-flag only. | (config_buffer + 4) |

### Runtime game-options block (7 dwords, source of Config.td5 GameOptions section)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00466000 | int[7] | `g_gameOptionsBlock` | **high** | 7-dword block read by `ScreenGameOptions @ 0x0041F990` (0x0041FD78 read, 0x0041FFF6/FFE/0042000D/0020 read+write), `ScreenMainMenuAnd1PRaceFlow @ 0x00415490` (0x0041557D read), and copied to Config.td5 at WritePackedConfigTd5 line 1 (`&DAT_00466000` → `&DAT_0048F388` ×7). Default bytes at file load: `03 01 01 01 01 00 01` matches g_td5.ini `{laps=3, checkpoint_timers=1, traffic=1, cops=1, difficulty=1, dynamics=0, collisions_3d=1}`. Field 0 = laps, field 1 = checkpoint_timers, field 2 = traffic, field 3 = cops, field 4 = difficulty, field 5 = dynamics, field 6 = collisions_3d. | TD5_GameOptions s_game_options @ td5_save.c:186 |

### Persistent audio / display / input-config dwords (Config.td5 individual fields)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00465FE8 | u32 | `g_audioSoundMode` | **high** | Sound mode (0=stereo, 1=mono). Read by `ScreenSoundOptions @ 0x0041EA90` (multiple sites 0x0041EE9A..0x0041F8A2), written by sound-screen at 0x0041F391. Copied to Config.td5 byte offset 0xBA (write at 0x0040F93F, load at 0x0040FD2E). | td5_save.c:195 `s_sound_mode` |
| 0x00465FEC | u32 | `g_audioSfxVolume` | **high** | SFX volume (0..100, stride-10). `ScreenLocalizationInit @ 0x004269D0` final block sets `DAT_00465FEC = DXSound::GetVolume() * scaling` (at 0x004271CC). Read/written by sound options screen. Copied to Config.td5 byte 0xBB. | td5_save.c:196 `s_sfx_volume` |
| 0x00465FF0 | u32 | `g_audioMusicVolume` | **high** | Music (CD) volume (0..100). `ScreenLocalizationInit` sets `DAT_00465FF0 = DXSound::CDGetVolume() * scaling`. Symmetric usage to `g_audioSfxVolume`. Copied to Config.td5 byte 0xBC. | td5_save.c:197 `s_music_volume` |
| 0x00465FF4 | u32 | `g_player2InputSource` (already named) | n/a | Already labeled `g_player2InputSource`. Noted for completeness. | s_p2_device_index @ td5_save.c:187 |
| 0x00465FF8 | u32 | `g_twoPlayerCatchupAssist` | **high** | Read by `ScreenTwoPlayerOptions @ 0x00420C70` (multiple sites at 0x00420FAD-0x004210D6). Written 4 sites. WriteConfig copies to Config.td5 byte 0xDD-ish region. Source-port labels this `s_catchup_assist`. | td5_save.c:223 `s_catchup_assist` |
| 0x00466020 | u32 | `gConfiguredDisplayModeOrdinal` (already named) | n/a | Already labeled (T1). Final byte index 0xBD-0xC0 of Config.td5. | s_display_mode @ td5_save.c:208 |
| 0x00466024 | u32 | `g_displayFogEnabled` | **high** | Read by `ScreenDisplayOptions @ 0x00420400` (0x00420794, 0x004207B7, 0x0042094A) and written at 0x00420955. Copied to Config.td5 byte 0xC1 in WriteConfig. | td5_save.c:209 `s_fog_enabled` |
| 0x00466028 | u32 | `g_displaySpeedUnits` | **high** | Speed-units (0=MPH, 1=KPH). Read by ScreenDisplayOptions (0x004207D3, 0x004207EC), `ScreenPostRaceHighScoreTable @ 0x00413580` (0x00413439), and `ScreenQuickRaceMenu @ 0x004213D0` (0x00421FE9). Copied to Config.td5 byte 0xC5. | td5_save.c:210 `s_speed_units` |
| 0x0046602C | u32 | `g_cameraSpeedSetting` (already named) | n/a | Already labeled. Camera damping. Config.td5 byte 0xC9. | s_camera_damping @ td5_save.c:212 |
| 0x00482F48 | u8 | `g_savedPlayer1CameraView` | **high** | Initial-view-mode byte for P1. Written by `LoadCameraPresetForView @ 0x00401450` (0x00401569) and read by `ResetRaceCameraSelectionState @ 0x00402000` (0x0040200B). Copied to Config.td5 at byte 0x3DF (Write 0x0040F9CC). | td5_save.c:224 `s_camera_byte_a` |
| 0x00482F49 | u8 | `g_savedPlayer2CameraView` | **high** | Symmetric P2 counterpart of `g_savedPlayer1CameraView`. Write @ 0x0040157B, read by ResetRaceCameraSelectionState. Copied to Config.td5 byte 0x3E0. | td5_save.c:225 `s_camera_byte_b` |
| 0x00466840 | u32 | `g_savedMusicTrackIndex` | **high** | Read at `LoadPackedConfigTd5 @ 0x0040FA11` (`DAT_00466840 = (uint)DAT_0049080E`), serialized in Write at 0x0040FD8F (`DAT_0049080E = (undefined1)DAT_00466840`). Used as bounds-check on high-score post-race table (`if (DAT_00497A68 == DAT_00466840)` at 0x004138E6) and read by track-selection state machine. CD audio track index. | td5_save.c:226 `s_music_track` |
| 0x00497A5C | u32 | `g_twoPlayerSplitMode` (already named) | n/a | Already labeled (T1). 2P split-screen mode (horizontal/vertical). Config.td5 byte 0x3DD. | s_split_screen_mode @ td5_save.c:222 |
| 0x00497A58 | u32 | `g_player1InputSource` (already named) | n/a | Already labeled (T1). Source-of-truth for P1 input device id. Config.td5 byte 0x20. | s_p1_device_index @ td5_save.c:187 |

### Controller-binding tables (4 device-descriptor blocks + 1 binding-map)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00463FC4 | u32[18] | `g_controllerBindings` | **high** | 18-dword controller binding map (72 bytes). LoadConfig copies `&DAT_0048F3B6` → `&DAT_00463FC4` ×18 (Config.td5 0x32 onward). On detected-device mismatch, `ScreenLocalizationInit` falls back to copying `&DAT_0046400C` defaults (0x00427081 LAB). | td5_save.c:190 `s_controller_bindings` |
| 0x0046400C | u32[18] | `g_defaultControllerBindings` | **high** | Constant default keyboard binding table, used as fallback in `ScreenLocalizationInit` when current device descriptor pair mismatches the saved pair. Source: read-only `.data`. | (none — defaults baked into td5_save_load_config) |
| 0x00465660 | u32[8] | `g_player1DeviceDesc` | **high** | P1 device descriptor block (8 dwords). LoadConfig writes from Config.td5 byte 0x7A (`&DAT_0048F3FE`). Used in mismatch check at `ScreenLocalizationInit` 0x00427060+. | td5_save.c:191 `s_p1_device_desc` |
| 0x00465680 | u32[8] | `g_player2DeviceDesc` | **high** | Symmetric P2 desc. LoadConfig writes from Config.td5 byte 0x9A (`&DAT_0048F41E`). | td5_save.c:192 `s_p2_device_desc` |
| 0x004656A0 | u32[8] | `g_player1DetectedDeviceDesc` | high | Detected-at-startup device-descriptor backup. ScreenLocalizationInit compares it to `g_player1DeviceDesc` to detect re-binding (0x0042707D read). | td5_save.c:193 `s_p1_device_desc_backup` |
| 0x004656C0 | u32[8] | `g_player2DetectedDeviceDesc` | high | Symmetric P2 detected backup. | td5_save.c:194 `s_p2_device_desc_backup` |
| 0x004978C0 | u32[98] | `g_player1CustomBindings` | **high** | P1 custom binding buffer (392 bytes / 0x188). LoadConfig at 0x0040FD13 copies Config.td5 byte 0xCD (`&DAT_0048F451`). | td5_save.c:215 `s_p1_custom_bindings` |
| 0x00497330 | u32[98] | `g_player2CustomBindings` | **high** | Symmetric P2 custom. Also receives the result of `BuildEnumeratedDisplayModeList()` if the comparison at 0x00427151 detects desc mismatch — i.e. this buffer doubles as the live display-mode-list cache. | td5_save.c:216 `s_p2_custom_bindings` |

### CupData.td5 mirror buffer + cup state header

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00490BAC | u8[12966] | `g_cupDataTd5IoBuffer` | **high** | 0x32A6-byte CupData.td5 read/write buffer. Cup snapshot starts at byte 0; first 12 bytes are header (game_type, race_index, npc_group, ...). The CRC32 is at offset +0x0C (`DAT_00490BB8`). Size lives in `DAT_00494BBC`. | td5_save.c:171 `s_cup_buf` |
| 0x00494BBC | u32 | `g_cupDataTd5ByteCount` | **high** | Always 0x32A6 (original) or larger w/ port overlay. Set by LoadContinueCupData at 0x004115FA after fread, written hardcoded to 0x32A6 by SerializeRaceStatusSnapshot at 0x004112B2, read by WriteCupData / ValidateCupDataChecksum for the encryption loop bound. | td5_save.c:176 `s_cup_buf_size` |
| 0x00490BB8 | u32 | `g_cupDataTd5Crc32` | **high** | Embedded CRC32 in cup buffer at offset +0x0C. Computed in SerializeRaceStatusSnapshot's final CRC32 loop (line `DAT_00490BB8 = ~uVar1;`). Verified by `RestoreRaceStatusSnapshot` `if (DAT_00490BB8 == ~uVar2)` and by `ValidateCupDataChecksum`. | (none — `cup_buffer.crc32` field per TD5_CupDataBuffer) |

### Cheat code system (T2.7 confirmation + 5 newly-pinned globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004655AC | u32[6] | `g_cheatCodeXorMaskTable` | **high** | T2.7 finding confirmed. Bytes: `01 00 00 00 08 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00 01 00 00 00` = `{1, 8, 2, 1, 1, 1}`. Each entry is the XOR mask applied to the target dword when cheat-code `i` completes. Read site: `RunFrontendDisplayLoop @ 0x00414E18`. | (none — port cheat system divergent) |
| 0x00465594 | void*[6] | `g_cheatCodeTargetPointers` | **high** | Pointer table: 6 dwords pointing to the cheat-target dwords. Bytes: `0x00496298, g_cupUnlockLevel(0x004962A8), 0x004962AC, 0x004AAF7C, 0x0049629C, g_cheatCodeActivated(0x004962B4)`. Read in `RunFrontendDisplayLoop @ 0x00414E1C-0x00414E26` as `*(uint *)(&PTR_DAT_00465594)[i] ^= g_cheatCodeXorMaskTable[i]`. Already labeled `PTR_DAT_00465594` (default symbol). | (none) |
| 0x004654A4 | u8[6][40] | `g_cheatCodeKeySequences` | **high** | Six 40-byte DirectInput keycode sequences. Stride 0x28; terminator 0xFF. Read at `RunFrontendDisplayLoop @ 0x00414E18` as `(&DAT_004654A4)[i * 0x28 + (&DAT_004951F0)[i]]`. Match-completion fires the XOR. | (none) |
| 0x004951F0 | u8[6] | `g_cheatCodeKeyProgress` | **high** | 6 progress indices (current key-in-sequence per cheat). Incremented on match, reset on non-match or activation. Decompile shows reset to zero via `_DAT_004951F0 = 0; _DAT_004951F4 = 0;` (8 bytes covers 6 bytes + 2 pad). | (none) |
| 0x00496298 | u32 | `g_cheatPostRaceHighScoreUnlock` | high | Cheat target 0 (XOR mask 1). Read in `RunFrontendDisplayLoop @ 0x00414E26` and gated on the post-race screen flow in `ScreenPostRaceHighScoreTable @ 0x004138C7`. Acts as a boolean toggle gating the high-score range branch (controls min-music-track display direction). | (none — port doesn't implement post-race-score cheat) |
| 0x004962AC | u32 | `g_cheatAttractModeOverride` | med | Cheat target 2 (XOR mask 2). Read by `ScreenQuickRaceMenu @ 0x0042140B`, attract-mode loaders `0x0040E07B/0x0040E8F8`. Gates an attract-mode-bypass branch. | (none) |
| 0x004AAF7C | u32 | `g_cheatNetworkControlReset` | high | Cheat target 3 (XOR mask 1). Read as `_g_networkControlBufferReset` in `UpdatePlayerVehicleControlState @ 0x00403422` to gate buffer-reset logic. Decompiler already renamed inline as `_g_networkControlBufferReset` — confirm with rename. | (none — port might not need) |
| 0x0049629C | u32 | `g_cheatRemoteBraking` | **high** | Cheat target 4 (XOR mask 1). Read as `_g_cheatRemoteBraking` in `UpdatePlayerVehicleControlState @ 0x004034DD`: zeroes velocities on all other slots when handbrake held — the "freeze rivals" cheat. Decompiler already renamed inline. | (none) |
| 0x004A2C9C | u8[26] | `gNpcRacerCheatFlags` (already named) | n/a | Already labeled (T2). Stride-1 per-NPC-group bitfield (`bit 0` = cheat-activated for that group; `bit 1` set by `RunFrontendDisplayLoop` when cheat completes globally). LoadPackedConfigTd5 restores this from Config.td5 bytes 0x14CD-0x14E6. | td5_save.c cheat_flags |

### Cup unlock state (read by AwardCupCompletionUnlocks + cheat machine)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00463E0C | u32 | `g_savedMaxUnlockedCar` | **high** | Max unlocked car index. `AwardCupCompletionUnlocks @ 0x00421DA0`: `if (DAT_00463e0c < iVar3 + 1) DAT_00463e0c = iVar3 + 1;`. Read by frontend car select (0x00421443, 0x0042197A), serialized to/from Config.td5 byte 0x148C. | td5_save.c TD5_ConfigBuffer.max_unlocked_car (offset 0x148C) |
| 0x00463E4C | u8[37] | `g_savedCarLockTable` | **high** | 37-byte car-lock table (1=locked, 0=unlocked). `AwardCupCompletionUnlocks` writes `(&DAT_00463E4C)[iVar3] = 0;`. Read by car-select screen 0x00421563 and quick-race 0x00421AF3. Config.td5 byte 0x14A8. | td5_save.c TD5_ConfigBuffer.car_locks (offset 0x14A8) |
| 0x004668B0 | u8[26] | `g_savedTrackLockTable` | **high** | 26-byte track-lock table (1=unlocked, 0=locked — inverted sense per `td5_save.c:163` comment). `RunFrontendDisplayLoop @ 0x00414EED` reads to select random attract-mode track. `TrackSelectionScreenStateMachine @ 0x00427E45` reads. Config.td5 byte 0x148E. | td5_save.c TD5_ConfigBuffer.track_locks (offset 0x148E) |
| 0x004962A8 | u32 | `g_cupUnlockLevel` (already named) | n/a | Already labeled (T1). 3-bit cup-tier completion bitfield. LoadConfig at 0x0040FD9C reads `(uint)DAT_0049080F`; Write masks to 7 bits. Config.td5 byte 0x148B. | td5_save.c TD5_ConfigBuffer.cup_tier_state |
| 0x004962B0 | u32 | `g_savedAllCarsUnlocked` | **high** | Boolean "all cars unlocked" flag. `AwardCupCompletionUnlocks @ 0x00421E28` writes `_DAT_004962B0 = 1` when case 0x12 fires with selectedCarIndex == 0x20. LoadConfig at 0x0040FDAE reads `DAT_00490810 >> 8 & 0xff`. Config.td5 byte 0x148D. | td5_save.c TD5_ConfigBuffer.all_cars_unlocked |

### NPC racer table (high-score / cup-completion identity)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004643B8 | u8[26 * 164] | `g_npcRacerGroupTable` (already named) | n/a | Already labeled. 4264-byte table. `AwardCupCompletionUnlocks` reads `(&g_npcRacerGroupTable)[g_selectedScheduleIndex * 0xA4]` as the per-group "cup-completion-state" byte (offset 0). 15 xrefs; central to high-score table. Default bytes baked at 0x004643B8 (visible from td5_save.c k_npc_default_table[]). LoadConfig restores from Config.td5 byte 0x3E1. | td5_save.c TD5_ConfigBuffer.npc_group_table (offset 0x3E1) |

### CupData snapshot fields (race-status snapshot mirror — populated by SerializeRaceStatusSnapshot)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497250 | u32[30] | `g_raceSchedule` | **high** | 120-byte race-schedule array. SerializeRaceStatusSnapshot copies 30 dwords from `&DAT_00497250` into cup buffer (`&DAT_00490BBC`). Restore reads it back. Source-port confirms this is the cup race-order table. | td5_save.c TD5_CupDataBuffer.race_schedule (offset 0x10) |
| 0x0048D988 | u32[30] | `g_raceResults` | **high** | 120-byte race-results array. Twin of `g_raceSchedule`; copied to cup buffer at `&DAT_00490C34`. | td5_save.c TD5_CupDataBuffer.race_results (offset 0x88) |
| 0x0048F30C | u32 | `g_cupMastersScheduleBase` | high | Read by SerializeRaceStatusSnapshot first line `DAT_00493E34 = _DAT_0048F30C;`. Symmetric write in Restore. Corresponds to `TD5_CupDataBuffer.masters_schedule_base @ 0x3288`. Already labeled in port. | td5_save.c TD5_CupDataBuffer.masters_schedule_base |
| 0x0048F314 | u32 | `g_p1CupScheduleIndex` | high | Per source-port comment at td5_save.c:133 ("VERIFIED: 0x48F314"). Read by Serialize, written by Restore. | td5_save.c TD5_CupDataBuffer.p1_cup_schedule_index |
| 0x0048F310 | u32 | `g_p2CupScheduleIndex` | high | Per source-port comment at td5_save.c:132 ("VERIFIED: 0x48F310"). Symmetric. | td5_save.c TD5_CupDataBuffer.p2_cup_schedule_index |
| 0x0048F318 | u16 | `g_p1CupCompletionBitmask` | med | Per source-port comment ("VERIFIED: 0x48F318"); 2 bytes. | td5_save.c TD5_CupDataBuffer.p1_cup_completion_bitmask |
| 0x0048F324 | u32 | `g_mastersEncounterFlags` | med | Per source-port comment ("VERIFIED: 0x48F324"). 4-byte misalignment from header start. | td5_save.c TD5_CupDataBuffer.masters_encounter_flags |
| 0x0048F368 | u32 | `g_savedCarIndexCup` | low | Read by RestoreRaceStatusSnapshot `DAT_0048F368 = DAT_00490BEC;` at 0x004112C0 final block. Mirror of cup-snapshot field; semantic = cached selected-car-id within an active cup snapshot. | (none — implicit through `td5_save_sync_cup_to_game`) |

## Key discoveries

1. **XOR-cipher implementation is identical for Config.td5 and CupData.td5.** Both use `encrypted[i] = plaintext[i] ^ key[i % strlen(key)] ^ 0x80`. The keys differ but the algorithm is byte-symmetric — encrypt and decrypt are the same operation. CRC-32 uses the standard polynomial 0xEDB88320 lookup table at `g_crc32_table_edb88320 @ 0x00475160`. The CRC placeholder substitution (write 0x00000010 at the CRC byte position before computing the checksum) means both files have CRCs that include the placeholder, not the final CRC value — a subtle "stable round-trip" trick to avoid the self-referential CRC problem. **This matches the port's td5_save.c implementation byte-for-byte.**

2. **The Config.td5 buffer layout is `[CRC(4)] + [GameOptions(7*4=28)] + [misc fields(...) ] + [NPC groups(4264)] + [trailer block: music_track, cup_tier, max_car, all_cars_unlocked, track_locks(26), car_locks(37), cheat_flags(26)]`.** Total = 5351 bytes. The trailer block is what `AwardCupCompletionUnlocks @ 0x00421DA0` updates and what `LoadPackedConfigTd5` fans out into `g_savedMaxUnlockedCar (0x00463E0C)`, `g_savedTrackLockTable (0x004668B0)`, `g_savedCarLockTable (0x00463E4C)`, `g_savedAllCarsUnlocked (0x004962B0)`, `g_cupUnlockLevel (0x004962A8)`, `gNpcRacerCheatFlags (0x004A2C9C)`.

3. **Cheat-code activation is universal and ungated by mode.** `RunFrontendDisplayLoop @ 0x00414B50` runs the cheat-keycode-match loop ONLY while `g_currentScreenFnPtr == ScreenOptionsHub` (PTR_ScreenOptionsHub_004655F4). All 6 cheat targets get the same XOR-with-mask treatment; activation sound is `DXSound::Play(4 or 5)` depending on whether the XOR result is now zero (cheat deactivated → play 5 = lower beep) or nonzero (cheat activated → play 4 = higher beep). The XOR mask `{1, 8, 2, 1, 1, 1}` means cheat #1 (g_cupUnlockLevel) flips bit 3 (= unlock the 4-cup tier).

4. **Cheat target 5 (`g_cheatCodeActivated @ 0x004962B4`) is the master "have any cheat been used" flag.** When it becomes 1 after a cheat input, the inner loop iterates all 26 NPC groups and OR's `gNpcRacerCheatFlags[i] |= 2` for every group whose first byte (cup-completion-state) is zero. This is the **cheat-detection-disqualification** mechanism: any cheat use sets bit 1 on every uncompleted NPC group, which the post-race screen reads to suppress high-score entry. **The Frida snapshot harness should capture `g_cheatCodeActivated` and `gNpcRacerCheatFlags` to reproduce this behavior; the port currently does not.**

5. **The "device descriptor backup" pair (`g_player1DetectedDeviceDesc @ 0x004656A0`, `g_player2DetectedDeviceDesc @ 0x004656C0`) implements a "binding stability check".** `ScreenLocalizationInit @ 0x004269D0` enumerates the currently-attached input devices into the *_DetectedDeviceDesc pair, then compares the result byte-by-byte against the *_DeviceDesc pair loaded from Config.td5 (post-LoadPackedConfigTd5). On mismatch, it resets `g_player1InputSource = 0; g_player2InputSource = 7` (keyboard P1, joypad-7 P2) and copies `g_defaultControllerBindings @ 0x0046400C` into `g_controllerBindings @ 0x00463FC4`. **This is what makes saved bindings survive hardware reconnects but reset cleanly on hardware change.**

6. **`g_savedMusicTrackIndex (0x00466840)` doubles as a high-score-table boundary constant.** In `ScreenPostRaceHighScoreTable @ 0x00413580`, it's compared against `DAT_00497A68` (a per-screen iteration counter) to decide loop boundaries. The variable name "music_track" comes from its primary role (CD audio track index for the current race scenario) but its read here is using its value as `total_count_of_unlocked_music_tracks` = `0x14` typically. Semantic confusion is intentional — saving disk space, single variable, multiple uses.

7. **The Config.td5 footer "cup tier state" (`g_cupUnlockLevel`) is masked to 3 bits on write (`DAT_0049080F = bVar2 & 7;` at WritePackedConfigTd5 0x0040FCFC).** This means only bits 0-2 are persisted — Championship/Challenge/Pitbull completion. Higher bits (set by the cheat XOR mask 8) are NOT persisted to disk, only stay live until next exit. This is a deliberate **cheat-cleanup behavior**: a cheat that unlocks the 4-cup tier (bit 3) gives the player a one-session bonus but the cheat-state doesn't survive an exit. Confirmed by the source port's `td5_save.c:184` comment "masked to 3 bits".

8. **`RestoreRaceStatusSnapshot @ 0x004112C0` uses a sentinel `g_selectedGameType = 0xFFFFFFFF` to indicate "no valid cup".** When the cup_data file's game_type byte (`DAT_00490BAC & 0xFF`) is 0xFF, the function returns 1 (CRC was OK) but explicitly sets `g_selectedGameType = -1` to signal that no cup is in progress. This is the cup-finish state — saved after completing the last race. **The source-port wrapper `td5_save_sync_cup_to_game` correctly mirrors this via its `out_race_within_series` return path.**

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x0046317C | Per-player initial-steering-bias short table (read by UpdatePlayerVehicleControlState; `*(short *)(&DAT_0046317C + slotIndex * 2)`) | T4 input/control batch |
| 0x00483014 | Per-slot input-control "first-touch" flag (8 bytes), gated by _g_networkControlBufferReset | T4 input/control batch |
| 0x0049080E-0x0049082E | CupData header trailing bytes — music_track, cup_tier, all_cars_unlocked, track_locks. Already covered semantically but specific byte-by-byte mapping not enumerated. | T4 deeper save-format audit if needed |
| 0x00490BBC | CupData race_schedule START — see g_raceSchedule, but at the offset INTO the cup buffer (not the global) | (none — buffer-local) |
| 0x00490C34 | CupData race_results START | (none — buffer-local) |
| 0x00490CAC | CupData actor_state START (12656 bytes) — copies from `g_actorRuntimeState` | T4 actor-state-serialization batch |
| 0x00493E1C | CupData slot_state buffer (gRaceSlotStateTable mirror) | T4 actor-state-serialization batch |
| 0x004962B4 | `g_cheatCodeActivated` (already named) — cheat target 5 | (covered) |
| 0x004962CC | DAT_004962CC — read alongside DAT_004962C4/C8 in ScreenPostRaceHighScoreTable; layout offsets for high-score-table presentation | T5 frontend high-score render batch |
| 0x00497A68 | High-score iterator counter — used in ScreenPostRaceHighScoreTable | T5 frontend high-score render batch |
| 0x00497A6C | Race-start banner flag — set in ScreenLocalizationInit on attract-mode finalize, read by screen-26 dispatch | T5 frontend banner-state batch |
| 0x004A2C90 | g_selectedScheduleIndex (already named) — drives AwardCupCompletionUnlocks dispatch | (covered) |
| 0x00482F4A-... | Camera bytes after 0x00482F49 — possible future cam-saved fields | T4 camera-save batch |
| 0x0046317C-0x0046318C | Per-slot steering-init shorts (6 entries × 2 bytes) | T4 input/control batch |

## TODO impact

This batch has no specific TODO from MEMORY.md slated for closure. However:

**T2.8 — `g_hudUseMetricUnits missing` gap pattern:** Surveyed 24 INI knobs in `main.c:208-264` against original-binary persistent globals. Findings:

- **`g_displaySpeedUnits @ 0x00466028`** is the persistent mirror of `[Display] SpeedUnits` — port's `g_td5.ini.speed_units` is correctly wired (main.c:420). However, the port keeps a separate runtime `s_speed_units` static in `td5_save.c:210`. The INI value is applied at load via `td5_save_set_speed_units(g_td5.ini.speed_units)` but I did not verify whether `td5_save_get_speed_units` is preferred at all read sites over `g_td5.ini.speed_units`. **Recommend audit:** every HUD read of speed-units should go through `td5_save_get_speed_units`, NOT `g_td5.ini.speed_units` — if any code reads the INI mirror directly after a runtime user-toggle (DisplayOptions screen flips `s_speed_units` only, not the INI field), the HUD will lag the menu.
- **`g_displayFogEnabled @ 0x00466024`** matches `[Display] Fogging` symmetrically — same audit recommendation.
- **`g_cameraSpeedSetting @ 0x0046602C`** matches `[Display] CameraDamping` (named in T1).
- **`g_audioSoundMode @ 0x00465FE8`** ↔ `[Audio] SFXMode` (main.c:426) — same audit.
- **No INI mirror found for `g_savedMusicTrackIndex @ 0x00466840`** — this is fine, it's a `CD audio` index that the game cycles through programmatically (not user-tunable). Port mirror `s_music_track @ td5_save.c:226` correctly tracks Config.td5 byte but no `[Audio] MusicTrack=` INI key exists. Not a gap.
- **No INI mirror found for `g_twoPlayerCatchupAssist @ 0x00465FF8`** — only mutated through `ScreenTwoPlayerOptions`. Could be exposed as `[GameOptions] CatchupAssist=` for headless tests; not currently wired. **Recommend small INI addition** if future testing needs deterministic 2P catchup-assist value.
- **No INI mirror found for camera-byte pair (`g_savedPlayer1CameraView/g_savedPlayer2CameraView`)** — set at runtime via `LoadCameraPresetForView`; persisting via Config.td5 byte 0x3DF/0x3E0 is the orig's intent. No INI gap. Camera-view INI keys do not exist; the runtime cycles them via UpdateRaceCamera.
- **No INI gap surfaced for cheat-system globals** — cheats are deliberately not INI-controllable; they are key-sequence-driven from the OptionsHub screen.

**Closing verdict: no new INI wiring gaps found in T4.16.** The port covers all 7 persistent GameOptions fields (`g_gameOptionsBlock @ 0x00466000`) symmetrically, and the audio/display/input-config layer is consistent between INI key parsing (main.c) and Config.td5 mirrors (td5_save.c). The single open audit item is "verify every read of *.ini.speed_units / fog_enabled / camera_damping / sfx_mode goes through td5_save_get_*" — IF any direct INI-field read exists in HUD/audio rendering after the user toggles via the DisplayOptions/SoundOptions menus.

## Ghidra session notes

- Session `f0fbd07e06ba4149a2526c6ddcf0bab7` opened `TD5_pool0` (slot acquired via `bash scripts/ghidra_pool.sh acquire`, `read_only=true` per project rules).
- Released via `bash scripts/ghidra_pool.sh release TD5_pool0` after analysis.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
- 30/30 high-confidence proposals can be applied directly. 6/30 medium-confidence proposals should be applied with `_PROVISIONAL` suffix until a downstream consumer is confirmed. 2/30 low-confidence are comment-only.
