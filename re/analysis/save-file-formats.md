# Test Drive 5 Save File Formats

Complete specification for `Config.td5` and `CupData.td5`.

---

## Encryption / Obfuscation

Both files use identical XOR encryption with different keys.

### Algorithm (shared)

```
For each byte at index i in the plaintext buffer:
    encrypted[i] = plaintext[i] XOR key[key_index] XOR 0x80
    key_index = (key_index + 1) % key_length
```

Decryption is identical (self-inverse):
```
plaintext[i] = encrypted[i] XOR key[key_index] XOR 0x80
```

The `0x80` flip means untouched zeroes encode as `0x80 XOR key[i]` rather than `key[i]` alone.

### Keys
- **Config.td5**: `"Outta Mah Face!! "` (18 chars, including trailing space, at VA `0x463F9C`)
- **CupData.td5**: `"Steve Snake says : No Cheating! "` (31 chars, including trailing space, at VA `0x464084`)

Key length is computed at runtime via `strlen()`, so the NUL terminator is NOT part of the key.

---

## CRC-32 Checksum

Both files use CRC-32 with the polynomial table at VA `0x475160` (standard CRC-32/ISO-HDLC, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`).

### Config.td5
- The first 4 bytes of the file are the CRC-32 of the **remaining** bytes.
- Write procedure:
  1. Serialize all fields into the buffer (0x48F384..0x49086A)
  2. Set bytes [0..3] to a placeholder (`0x10, 0x00, 0x00, 0x00`)
  3. Compute CRC-32 over the ENTIRE buffer (including the placeholder bytes)
  4. Overwrite bytes [0..3] with the CRC-32 result
  5. XOR-encrypt the entire buffer
  6. Write to file
- Load procedure:
  1. Read file, XOR-decrypt
  2. Save the first 4 bytes (stored CRC)
  3. Replace first 4 bytes with `0x10, 0x00, 0x00, 0x00`
  4. Compute CRC-32 over the entire buffer
  5. Compare: if `computed != stored`, reject (return 0)

### CupData.td5
- The CRC-32 is stored INSIDE the serialized buffer at offset +0x0C (4 bytes), not at the file start.
- The CRC is computed over the entire 0x32A6-byte buffer (including the CRC field set to its placeholder).
- `ValidateCupDataChecksum` (VA `0x411630`) accepts 3 params: the third is the expected CRC. It re-reads the file, decrypts, computes CRC-32 over the full buffer, and compares.

---

## Config.td5 Format

**Writer:** `WritePackedConfigTd5` (VA `0x40F8D0`)
**Reader:** `LoadPackedConfigTd5` (VA `0x40FB60`)
**Trigger:** Called from `InitializeFrontendDisplayModeState` every time the user exits an options screen to the main menu.
**Load:** Called once from `ScreenLocalizationInit` at startup.
**Buffer:** `0x48F384` through `0x49086A` (total **0x14E7 = 5351 bytes**)
**Read buffer max:** `0x1800` (6144 bytes) -- safely oversized

### Corruption / Missing File Behavior
- If `fopen("Config.td5", "rb")` fails: returns 0 (no config loaded; game uses defaults).
- If CRC mismatch after decryption: returns 0 (same as missing file).
- All globals retain their compile-time default values.

### Field Layout (file offsets)

| Offset | Size | Source Global(s) | Description |
|--------|------|-----------------|-------------|
| 0x0000 | 4 | (computed) | **CRC-32** of entire buffer (with this field = `0x10000000` during computation) |
| 0x0004 | 28 (7 dwords) | `0x466000`..`0x46601C` | **Game Options** (see breakdown below) |
| 0x0020 | 1 | `0x497A58` (low byte) | **Player 1 controller device index** (index into device table at `0x465660`) |
| 0x0021 | 1 | `0x465FF4` (low byte) | **Player 2 controller device index** |
| 0x0022 | 4 | `0x464054` | **Force feedback configuration dword A** (raw DInput effect params) |
| 0x0026 | 4 | `0x46405C` | **Force feedback configuration dword B** |
| 0x002A | 4 | `0x464058` | **Force feedback configuration dword C** |
| 0x002E | 4 | `0x464060` | **Force feedback configuration dword D** |
| 0x0032 | 72 (0x12 dwords) | `0x463FC4`..`0x464014` | **Controller binding tables** -- 18 dwords of key/button mappings for P1 and P2 (9 actions x 2 players) |
| 0x007A | 32 (8 dwords) | `0x465660`..`0x46567C` | **P1 active controller device descriptor** (device type+caps, 0x20 bytes) |
| 0x009A | 32 (8 dwords) | `0x465680`..`0x46569C` | **P2 active controller device descriptor** (device type+caps, 0x20 bytes) |
| 0x00BA | 1 | `0x465FE8` (low byte) | **Sound mode** (0=Stereo, 1=Surround, 2=3D) |
| 0x00BB | 1 | `0x465FEC` (low byte) | **SFX volume** (0-100, step 10) |
| 0x00BC | 1 | `0x465FF0` (low byte) | **Music/CD volume** (0-100, step 10) |
| 0x00BD | 4 | `0x466020` | **Display mode ordinal (raw)** -- internal M2DX display mode index |
| 0x00C1 | 4 | `0x466024` | **Fogging enabled** (0=Off, 1=On) |
| 0x00C5 | 4 | `0x466028` | **Speed readout units** (0=MPH, 1=KPH) |
| 0x00C9 | 4 | `0x46602C` | **Camera damping** (0-9, controls chase cam smoothing) |
| 0x00CD | 392 (0x62 dwords) | `0x4978C0`..`0x497A54` | **Player 1 controller custom binding map** (detailed axis/button config, 0x188 bytes) |
| 0x0255 | 392 (0x62 dwords) | `0x497330`..`0x4974C4` | **Player 2 controller custom binding map** (0x188 bytes) |
| 0x03DD | 1 | `0x497A5C` (low byte) | **Split-screen mode** (0=Horizontal, 1=Vertical) |
| 0x03DE | 1 | `0x465FF8` (low byte) | **2P catch-up assist** (0-9) |
| 0x03DF | 1 | `0x482F48` | **Unknown byte A** (camera-related, written from `UpdateChaseCamera`) |
| 0x03E0 | 1 | `0x466840` (low byte) | **Music track selection** (CD audio track index) |
| 0x03E1 | 4264 (0x42A dwords) | `0x4643B8`..`0x464E5F` | **NPC Racer Group Table** (high scores) -- 26 groups x 164 bytes (see breakdown below) |
| 0x148A | 1 | (constant 0) | **Reserved / padding byte** (always written as 0) |
| 0x148B | 1 | `0x4962A8` & 0x7 (low 3 bits) | **2P mode state** (masked to 3 bits) |
| 0x148C | 1 | `0x463E0C` (low byte) | **Max unlocked car index** (highest car slot unlocked, 0x00-0x20) |
| 0x148D | 1 | `0x4962B0` (low byte) | **All-cars-unlocked flag** (set to 1 by Ultimate cup completion) |
| 0x148E | 26 (6 dwords + 2 bytes) | `0x4668B0`..`0x4668C9` | **Track lock table** (26 bytes, one per track: 0=locked, 1=unlocked. First 8 default to 0, next 18 default to 1) |
| 0x14A8 | 37 (9 dwords + 1 byte) | `0x463E4C`..`0x463E74` | **Car lock table** (37 bytes, one per car: 0=unlocked, 1=locked. First ~16 default to 0, rest to 1) |
| 0x14CD | 26 (0x1A bytes) | `0x4A2C9C`..`0x4A2CB5` | **Cheat activation flags** (1 byte per NPC group, bit 0 = group-unlock cheat active, bit 1 = cheat-all flag) |

**Total: 0x14E7 bytes (5351)**

### Game Options Breakdown (offset 0x0004, 7 dwords)

| Dword | Global | Menu Label | Values |
|-------|--------|-----------|--------|
| 0 | `0x466000` | Circuit Laps | 0-3 (maps to 2/4/6/8 laps) |
| 1 | `0x466004` | Checkpoint Timers | 0=Off, 1=On |
| 2 | `0x466008` | Traffic | 0=Off, 1=On |
| 3 | `0x46600C` | Cops | 0=Off, 1=On |
| 4 | `0x466010` | Difficulty | 0=Easy, 1=Normal, 2=Hard (wraps) |
| 5 | `0x466014` | Dynamics | 0=Arcade, 1=Simulation |
| 6 | `0x466018` | 3D Collisions | 0=Off, 1=On |

### NPC Racer Group Table (offset 0x03E1, 26 groups)

Each group is 164 bytes (0xA4):

| Group Offset | Size | Description |
|-------------|------|-------------|
| 0x00 | 4 | Header byte (0=standard NPC group, 1=unlockable special) |
| 0x04 | 32 | Entry 0: NPC racer (name[13] + pad + car_sprite_id[4] + car_index[4] + best_lap[4] + best_race[4]) |
| 0x24 | 32 | Entry 1 |
| 0x44 | 32 | Entry 2 |
| 0x64 | 32 | Entry 3 |
| 0x84 | 32 | Entry 4 |

Each 32-byte entry:
- `[0x00..0x0C]` Name (13 bytes, NUL-padded)
- `[0x0D..0x0F]` Padding
- `[0x10..0x13]` Car sprite/model ID
- `[0x14..0x17]` Car index
- `[0x18..0x1B]` Best lap time (milliseconds)
- `[0x1C..0x1F]` Best race time (milliseconds)

### Load Asymmetry: Controller Device Descriptors

The writer reads P1 from `0x465660` and P2 from `0x465680` (active device slots 0 and 1).
The reader writes P1 to `0x4656A0` and P2 to `0x4656C0` (configured backup slots 2 and 3).
This is intentional: the active slots are populated at runtime from hardware enumeration; the backup slots preserve the saved configuration for re-binding.

### Cheat Code System

Six cheat codes can be typed on the Options Hub screen. Each code is a sequence of DInput scancodes stored in tables at VA `0x4654A4` (6 entries of 0x28 bytes, terminated by `0xFF`).

The cheat targets are pointer/XOR-mask pairs:
- Table of 6 target pointers at `0x465594`
- Table of 6 XOR masks at `0x4655AC`

When a cheat is entered, `target ^= mask` toggles the value. The state of the cheat flags is persisted in Config.td5 at offset `0x14CD` (26 bytes at `0x4A2C9C`), with bit 0 recording group unlock and bit 1 recording cheat-all activation.

---

## CupData.td5 Format

**Writer:** `WriteCupData` (VA `0x4114F0`)
**Serializer:** `SerializeRaceStatusSnapshot` (VA `0x411120`)
**Reader:** `LoadContinueCupData` (VA `0x411590`)
**Deserializer:** `RestoreRaceStatusSnapshot` (VA `0x4112C0`)
**Validator:** `ValidateCupDataChecksum` (VA `0x411630`)
**Trigger:** Written from `RunRaceResultsScreen` after each race in a cup series.
**Load:** Called from `RaceTypeCategoryMenuStateMachine` when "Continue" is selected.
**Buffer:** `0x490BAC` through `0x493E51` (total **0x32A6 = 12966 bytes**, exact)
**Read buffer max:** `0x4000` (16384 bytes) -- safely oversized
**Size stored at:** `DAT_00494BBC` (set to `0x32A6` by serializer)

### Corruption / Missing File Behavior
- If `fopen("CupData.td5", "rb")` fails: returns silently (no cup data loaded).
- After decryption, the CRC-32 embedded at offset +0x0C is validated against the full buffer.
- If CRC mismatch: `RestoreRaceStatusSnapshot` returns 0, cup continuation is rejected.
- If `g_selectedGameType` decodes to `0xFF`, it is remapped to `-1` (no active game type).

### Serialization Flow

1. `SerializeRaceStatusSnapshot` copies all cup-state globals into the contiguous buffer at `0x490BAC`.
2. Sets the first 4 bytes at `0x490BB8` to placeholder `0x10, 0x00, 0x00, 0x00`.
3. Computes CRC-32 over the full 0x32A6-byte buffer.
4. Stores CRC at `0x490BB8` (offset +0x0C).
5. Stores the size `0x32A6` at `DAT_00494BBC`.
6. `WriteCupData` XOR-encrypts a stack copy and writes to file.

### Field Layout (file offsets)

| Offset | Size | Source Global(s) | Description |
|--------|------|-----------------|-------------|
| 0x0000 | 1 | `g_selectedGameType` (`0x49635C`) | **Game type** (1-9; see game mode table) |
| 0x0001 | 1 | `g_raceWithinSeriesIndex` (`0x494BB8`) | **Current race index** within the cup series (0-based) |
| 0x0002 | 1 | `DAT_004A2C90` | **NPC group index** for the current cup (0-25) |
| 0x0003 | 1 | `DAT_00490BA8` | **Track/opponent schedule state** |
| 0x0004 | 1 | `gRaceRuleVariant` (`0x4AAF70`) | **Race rule variant** (0-5, maps to cup type behavior) |
| 0x0005 | 1 | `gTimeTrialModeEnabled` (`0x4AAF6C`) | **Time trial flag** (0 or 1) |
| 0x0006 | 1 | `gWantedModeEnabled` (`0x4AAF68`) | **Cop chase / wanted flag** (0 or 1) |
| 0x0007 | 1 | `gRaceDifficultyTier` (`0x463210`) | **Difficulty tier** (0=Easy, 1=Normal, 2=Hard) |
| 0x0008 | 1 | `DAT_004B0FA8` | **Checkpoint mode flag** |
| 0x0009 | 1 | `gTrafficActorsEnabled` (`0x4AAD8C`) | **Traffic enabled** (0 or 1) |
| 0x000A | 1 | `gSpecialEncounterEnabled` (`0x46320C`) | **Special encounter gate** (0 or 1) |
| 0x000B | 1 | `gCircuitLapCount` (`0x466E8C`) | **Circuit lap count** for this cup |
| 0x000C | 4 | (computed) | **CRC-32** of entire 0x32A6-byte buffer |
| 0x0010 | 120 (0x1E dwords) | `0x497250`..`0x4972C7` | **Race schedule table** (track IDs, order for each race in the series; 30 dwords) |
| 0x0088 | 120 (0x1E dwords) | `0x48D988`..`0x48D9FF` | **Race results table** (per-race results/scores for the cup) |
| 0x0100 | 12624 (0xC5C dwords) | `gRuntimeSlotActorTable` (`0x4AB108`) | **Full actor state** for all 6 racing slots (6 x 0x388 bytes = 0x1510 bytes per slot... 0x3170 total = 0xC5C dwords) |
| 0x3270 | 24 (6 x 4 bytes) | `gRaceSlotStateTable` (`0x4AADF4`) | **Slot state entries** (6 slots, 4 bytes each: companion state, finish flags) |
| 0x3288 | 4 | `0x48F30C` | **Masters mode: opponent schedule array base** (first entry) |
| 0x3290 | 4 | `0x48F314` | **Cup sub-state dword A** |
| 0x3294 | 4 | `0x48F310` | **Cup sub-state dword B** |
| 0x3296 | 1 | `0x48F31A` | **Cup sub-state byte A** |
| 0x3297 | 2 | `0x48F318` | **Cup sub-state word A** |
| 0x329B | 4 | `0x48F328` | **Cup sub-state dword C** |
| 0x329F | 4 | `0x48F324` | **Masters mode encounter flags** (15-byte array base) |
| 0x32A3 | 2 | `0x48F330` | **Cup sub-state word B** |
| 0x32A5 | 1 | `0x48F332` | **Cup sub-state byte B** |

**Total: 0x32A6 bytes (12966)**

### Additional Context Fields (restored but not in contiguous layout)

During restore, `RestoreRaceStatusSnapshot` also recovers:
- `DAT_0048F364` -- cup progress marker (from early buffer, `DAT_00490BD4` at restore offset +0x28)
- `DAT_0048F368` -- from `DAT_00490BEC` (offset +0x40)
- `DAT_0048F370` -- from `DAT_00490C04` (offset +0x58)
- `DAT_0048F378` -- from `DAT_00490C1C` (offset +0x70)

These are within the race schedule table region and represent cross-references into the schedule.

### Game Type Values in CupData

| Value | Mode |
|-------|------|
| 1 | Championship |
| 2 | Era Challenge |
| 3 | Challenge |
| 4 | Pitbull |
| 5 | Masters |
| 6 | Ultimate |
| 7 | Time Trial |
| 8 | Cop Chase |
| 9 | Drag Race |
| 0xFF | No active type (remapped to -1 on load) |

---

## Unlock Progression

### Car Unlocks

- **Car lock table** at `0x463E4C` (37 bytes): each byte is 0 (unlocked) or 1 (locked).
- **Max unlocked car index** at `0x463E0C`: the highest car slot index that has been unlocked. Cars at indices <= this value with lock byte 0 are available.
- Default state: first ~16 cars unlocked (bytes = 0), remaining 21 locked (bytes = 1).
- `AwardCupCompletionUnlocks` (VA `0x421DA0`) unlocks specific cars upon completing specific cup groups:
  - Group 4 -> unlocks car 0x15
  - Group 5 -> unlocks car 0x11
  - Group 6 -> unlocks car 0x18
  - Group 7 -> unlocks car 0x19
  - Group 0x10 -> unlocks car 0x17
  - Group 0x11 -> unlocks car 0x1A
  - Group 0x12 -> unlocks car 0x20 (or sets all-cars flag if already at 0x20)
- **All-cars-unlocked flag** at `0x4962B0`: when set to 1 by Ultimate cup completion, all cars become available.

### Track Unlocks

- **Track lock table** at `0x4668B0` (26 bytes): 0=locked, 1=unlocked.
- Default: first 8 tracks locked (0x00), remaining 18 unlocked (0x01).
- Tracks are unlocked by completing NPC groups with header byte 0 (standard groups). The cheat system can also toggle unlock state.

---

## Summary

| Property | Config.td5 | CupData.td5 |
|----------|-----------|-------------|
| File size | 5351 bytes | 12966 bytes |
| XOR key | "Outta Mah Face!! " (18 chars) | "Steve Snake says : No Cheating! " (31 chars) |
| CRC location | First 4 bytes | Offset +0x0C |
| CRC placeholder | `0x10, 0x00, 0x00, 0x00` | `0x10, 0x00, 0x00, 0x00` |
| Written when | Exit options to menu | After each cup race |
| Read when | Game startup | "Continue" selected |
| Missing file | Use defaults | No cup continuation |
| Bad CRC | Use defaults | Cup continuation rejected |
| Content | Settings, controls, unlocks, high scores, cheats | Full mid-cup state snapshot |
