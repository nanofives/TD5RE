# Test Drive 5: Patchable Limits Catalog

**Date:** 2026-03-28
**Binary:** TD5_d3d.exe (+ M2DX.dll for sound)
**Scope:** Every hardcoded limit and magic number suitable for modding, categorized by subsystem.
**Excludes:** Resolution constants (see `hardcoded-resolution-audit.md`) and tick-rate constants (see `tick-rate-dependent-constants.md`).

---

## GAMEPLAY LIMITS

### GL-1. Max Race Actors (6)

The race actor array is a statically-allocated block at `0x4AB108` with stride `0x388` (904 bytes per actor). The first 6 slots (indices 0-5) are race actors (player + AI opponents).

**Array layout:**
- Actor 0 starts at `0x4AB108`
- Actor 5 ends at `0x4AC9B0`
- Total race actor region: `0x4AB108` to `0x4AC9B0` (6 x 0x388)

**Addresses where the limit appears:**

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x42F1EB` | `CMP ESI,0x4AC9B0` | InitializeRaceVehicleRuntime: end-of-race-actor boundary |
| `0x42F5B0+` | `iVar2 < 5` (5 sort passes) | UpdateRaceOrder: bubble sort over 6 positions |
| `0x42F5B0+` | `iVar2 < 6` (position assignment) | UpdateRaceOrder: assign place to 6 actors |
| `0x42F6C0` | `CMP EAX,0x6` | Race init: gear count check per actor |
| `0x42F7C7` | `CMP EDX,0x6` | Race init: gear count check |
| `0x42F880` | `CMP dword [ESP+0x28],0x6` | Race init: per-actor loop bound |
| `0x42F928` | `CMP dword [ESP+0x28],0x6` | Race init: per-actor loop bound |
| `0x440B00+` | `uVar10 < 6` | UpdateRaceSoundMixer: sound per-actor inner loop |
| `0x42B580+` | `pcVar9 < 0x4AAE0C` | RunRaceFrame: actor state array traversal (DAT_004AADF4, stride 4, 6 entries) |

**Interdependencies:**
- The static actor array at `0x4AB108` is in BSS -- enlarging it requires relocating the data segment or using a heap allocation.
- The race order array at `0x4AE278` holds 6 byte-sized position entries.
- The sound mixer allocates 3 DXSound channels per race actor (engine idle, engine rev, horn/collision), totaling 18 channels for 6 actors. Adding actors requires more sound channels.
- All 30 occurrences of stride `0x388` in the code operate on this array.
- The AI rubber-banding system references actor indices 0-5.
- `DAT_004AADF4` (actor type flags) is a 6-entry dword array (24 bytes, range `0x4AADF4` to `0x4AAE0C`).

**Safe patch range:** Cannot increase beyond 6 without relocating the BSS actor array and expanding every boundary check. The traffic actor region immediately follows.

**Risk: HARD** -- Requires BSS relocation, expanding multiple parallel arrays, adjusting sound channel allocation.

---

### GL-2. Max Traffic Actors (variable, up to ~8)

Traffic actors occupy the next portion of the actor array after race actors, from `0x4AC9B0` to approximately `0x4AE5F0`.

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x42F274` | `CMP ESI,0x4AE5F0` | InitializeRaceVehicleRuntime: end-of-traffic boundary |
| `0x42F337` | `CMP ESI,0x4AC7F4` | Carparam tuning loop end address |
| `0x42F3B0` | `CMP ESI,0x4AC7F4` | Carparam tuning loop (easy difficulty) |
| `0x440B00+` | `6 < DAT_004AAF00` | Sound mixer: traffic sound starts at actor index 6 |
| `0x440B00+` | `local_30 < DAT_004AAF00` | Sound mixer: traffic actor loop bound (dynamic) |

**Key global:** `DAT_004AAF00` holds the runtime total actor count (race + traffic). The traffic count is `DAT_004AAF00 - 6`.

**Safe patch range:** The boundary addresses are hardcoded as absolute pointers into BSS. Cannot increase without relocating the actor array.

**Risk: HARD** -- Same issues as GL-1.

---

### GL-3. Max Cup Races per Series (4-12)

Cup race schedules are stored in a static table at `0x4640A4`:

| Cup | Game Type | Races | Track indices (terminated by 0x63) |
|-----|-----------|-------|------------------------------------|
| Championship | 1 | 4 | 0, 1, 2, 3 |
| Era | 2 | 6 | 4, 16, 6, 7, 5, 17 |
| Challenge | 3 | 6 | 0, 1, 2, 3, 15, 8 |
| Pitbull | 4 | 8 | 0, 1, 2, 3, 15, 8, 11, 13 |
| Masters | 5 | 10 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12 |
| Ultimate | 6 | 12 | 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 9, 14 |

**Addresses:**

| Address | What |
|---------|------|
| `0x4640A4` (data) | Cup schedule table base |
| `0x466894` (data) | gScheduleToPoolIndex mapping table |
| `0x466D50` (data) | Pool-to-ZIP filename table |

**Safe patch range:** Each schedule is a byte array terminated by 0x63. The schedules are packed sequentially. Adding more races to a cup requires either:
1. Overwriting later cup data (shifts all subsequent cups)
2. Redirecting the schedule pointer to new memory

The Ultimate cup already uses 12 of the ~18 available tracks. Max theoretical races = total track count.

**Risk: MODERATE** -- Data-only patch to the schedule table. The cup results screen and save system must handle the new race count. The post-race results scoring array at `DAT_004AE280` (stride 0x80) has fixed capacity per game type.

---

### GL-4. Max Track Pool Size (18 tracks)

Track indices reference a pool of track definitions. The pool-to-ZIP table at `0x466D50` and the schedule-to-pool mapping at `0x466894` define the available tracks. The game ships with 18 track variants (indices 0-17).

**Addresses where track count matters:**

| Address | Context |
|---------|---------|
| `0x427630+` | TrackSelectionScreenStateMachine: iterates available tracks |
| `0x466D50` (data) | Track ZIP filename table |
| `0x466894` (data) | Schedule-to-pool index map |
| `0x4640A4` (data) | Cup schedules reference track indices |

**Safe patch range:** Adding tracks requires new ZIP files with level data, extending the filename table, and expanding the schedule/pool mapping tables. The table sizes are not explicitly bounded in code -- they are terminated by sentinel values or bounded by game type logic.

**Risk: MODERATE** -- Requires new asset files plus data table expansion. No code changes needed if tables can be extended in-place.

---

### GL-5. Max Car Models (data-driven)

Car selection is driven by `carparam.dat` entries and the car ZIP archive table. The CarSelectionScreenStateMachine at `0x40DFC0` iterates available cars based on the loaded configuration.

**Key data:**
- `carparam.dat`: Per-vehicle physics tuning (stride ~0x80 per entry)
- Car ZIP table: Maps car indices to archive filenames
- The selection screen creates preview buttons dynamically

**Risk: EASY** -- Car count is data-driven from `carparam.dat`. Adding cars requires new car ZIP archives and extending carparam.dat. No hardcoded car count limit found in the EXE.

---

## DISPLAY LIMITS

*Resolution limits are fully documented in `hardcoded-resolution-audit.md`. Only non-resolution display limits are listed here.*

### DL-1. Depth Sort Bucket Count (4096)

The 3D render pipeline uses bucket sort for depth ordering. The bucket array is at `DAT_004BF6C8` with exactly 0x1000 (4096) entries.

**Addresses:**

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x43E2F0+` | `local_4 = 0x1000; do { ... } while (local_4 != 0)` | FlushDepthSortBuckets: iterates all 4096 buckets |
| `0x43E7E6` | `MOV [0x467368],0x1000` | ConfigureProjectionForViewport: sets depth range to 0x1000 |
| `0x43E2C0` (data ref) | `DAT_004BF6C8` | Bucket array pointer (allocated dynamically) |
| Multiple at `0x4023xx` | `MOV [0x467368],0x1000` | Various depth-range initializers |

The bucket array is a statically-addressed pointer table (4096 x 4 bytes = 16 KB). The node pool at `DAT_004BF6C4` is allocated as 0x10000 (65536) bytes.

**Safe patch range:** Increasing bucket count improves depth precision (less Z-fighting). The bucket array at `DAT_004BF6C8` is a 4096-entry pointer array (16 KB). To increase, both the bucket count AND the depth range constant at `0x467368` must match. The node pool (64 KB) may also need enlarging for scenes with many polygons.

**Risk: MODERATE** -- Must patch all locations that write 0x1000 to `DAT_00467368` (at least 8 sites) plus the bucket iteration loop. The bucket pointer array is statically allocated in BSS.

---

### DL-2. Render Node Pool (512 nodes)

The render node pool is initialized at `0x4312E0` with 0x200 (512) nodes, each 8 bytes (2 DWORDs), allocated from a 0x1000-byte block.

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x4312E0+` | `iVar2 = 0x200` | Node pool init: creates 512 linked list nodes |
| `0x4312E0+` | `FUN_00430CF0(0x1000)` | Allocates 4096 bytes for node pool |

**Safe patch range:** Increasing beyond 512 nodes requires a larger allocation. Each additional node costs 8 bytes. The pool serves the depth-sort linked lists.

**Risk: EASY** -- Change the allocation size and loop count. Both are in a single function.

---

### DL-3. Sort Node Pool (64 KB)

The depth sort node pool for polygon insertion is allocated at `0x43E5F0`:

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x43E5F0+` | `FUN_00430CF0(0x10000)` | Allocates 65536 bytes for sort nodes |
| `0x43E5F0+` | `FUN_00430CF0(0x801F)` | Allocates ~32 KB for vertex buffer |
| `0x43E5F0+` | `FUN_00430CF0(0x41F)` | Allocates ~1 KB for auxiliary buffer |

**Safe patch range:** The 64 KB pool limits total polygon count per frame. If scenes overflow, polygons are dropped. Doubling to 128 KB would allow twice as many sorted primitives.

**Risk: EASY** -- Single allocation size change.

---

## AUDIO LIMITS

### AL-1. DXSound Base Buffer Slots (44)

M2DX.dll's DXSound system has exactly 44 base buffer slots (indices 0-43) plus 44 duplicate slots (indices 44-87) for polyphonic overlap, totaling 88 channel entries.

**Addresses in TD5_d3d.exe:**

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x441D50+` | `iVar1 = 0x2C; ... iVar1 < 0x58` | StopAllRaceSounds: stops channels 44-87 (duplicate range) |
| `0x441D50+` | `iVar1 = 0; ... iVar1 < 0x2C` | StopAllRaceSounds: removes channels 0-43 (base range) |
| `0x440B00+` | `iVar5 = 0x2C; ... iVar5 < 0x58` | UpdateRaceSoundMixer: stops duplicate channels |
| `0x440B9C` | `MOV ESI,0x2C` | Sound channel base offset |
| `0x441A1D` | `MOV [EBP-0x1C],0x2C` | Sound init: channel base |
| `0x441F6C` | `MOV EBX,0x2C` | Sound load: base channel count |

**Channel allocation scheme:**
- Channels 0-43 (0x00-0x2B): Base sound buffers (loaded WAVs)
- Channels 44-87 (0x2C-0x57): Duplicate buffers for overlapping playback
- Per race actor: 3 channels (engine idle, engine rev, horn/collision) x 6 actors = 18 base channels
- Per traffic actor: 1 channel each, starting at DXSound slot `actor_index * 3 + base_offset`
- Police siren: 2 channels (per player in split-screen)
- Ambient: remaining channels

**In M2DX.dll:**
- `g_soundBufferTable` at `0x1005F974`: stride 0x30 per slot, 88 total slots
- The capacity 44/88 is hardcoded in the DLL's `Init`, `Play`, `Stop`, `Remove`, and `Status` functions.

**Safe patch range:** Increasing requires patching M2DX.dll's buffer table size AND all loop bounds in both the DLL and EXE. The DLL uses `slot * 12` (12 DWORDs = 0x30 bytes) for array indexing.

**Risk: HARD** -- Requires coordinated patches across two binaries (TD5_d3d.exe + M2DX.dll). The DLL's static buffer table must be enlarged.

---

### AL-2. Sound Channels per Race Actor (3)

Each race actor uses 3 DXSound channels: engine idle (gear 1), engine rev (current gear), and horn/collision SFX.

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x440B00+` | `uVar10 * 3 + local_20` | Channel index = actor * 3 + player_offset |
| `0x440B00+` | `local_20 + 0x14` (collision) | Collision sound = player_offset + 0x14 |
| `0x440B00+` | `local_20 + 0x15` (siren A) | Police siren channel A |
| `0x440B00+` | `local_20 + 0x16` (siren B) | Police siren channel B |

The `* 3` multiplier is used throughout the sound mixer to compute channel indices.

**Safe patch range:** Fixed by the 3-channel-per-actor design. Cannot add more sound layers per actor without restructuring the entire channel allocation scheme.

**Risk: HARD** -- Pervasive `* 3` multiplier throughout sound code.

---

## PARTICLE / VISUAL EFFECTS LIMITS

### VE-1. Max Particles per Player Viewport (100)

Each player viewport has 100 particle slots (stride 0x40 = 64 bytes each). The particle pool base is at `DAT_004A318F` with 2 viewports x 100 particles = 200 total slots. Total particle memory = 100 x 0x40 x 2 = 12800 bytes (0x3200).

**Addresses:**

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x429720+` | `iVar1 < 100` (0x64) | DrawRaceParticleEffects: iterates 100 particles |
| `0x429780` | `CMP EDI,0x64` | Particle callback dispatch: loop bound |
| `0x4297BC` | `CMP EDI,0x64` | Particle update: loop bound |
| `0x429A30+` | `iVar8 < 100` | SpawnVehicleSmokeVariant: find free slot in 100 entries |
| `0x429A53` | `CMP ESI,0x64` | Smoke variant: particle limit |
| `0x429D2C` | `CMP ESI,0x64` | Smoke variant B: particle limit |
| `0x429FF3` | `CMP ESI,0x64` | Smoke variant C: particle limit |
| `0x42A2E8` | `CMP ESI,0x64` | Smoke variant D: particle limit |
| `0x42A6F5` | `CMP ESI,0x64` | Smoke variant E: particle limit |
| `0x4296B6` | `MOV EDI,0x64` | Particle init: set count to 100 |

**Per-particle active flag array:** `DAT_004A6370` (0x32 = 50 bytes per viewport, but actually 100 1-byte entries -- the `param_4 * 0x32` offset is per viewport, so there are `0x32` = 50 active flag bytes per player slot, suggesting 50 visible particle sprites max per viewport with the remainder as state-only).

**Safe patch range:** Increasing to 128 or 200 particles requires expanding the particle array at `DAT_004A318F` (currently 100 x 0x40 = 0x1900 per viewport). All 10+ CMP instructions with 0x64 must be patched. The active flag array at `DAT_004A6370` (0x32 entries per viewport) would also need expansion.

**Risk: MODERATE** -- Many addresses to patch but all are simple immediate values. The particle array is in BSS so size depends on available space.

---

### VE-2. Max Tire Track Segments (80)

The tire track pool is managed by `UpdateTireTrackPool` at `0x43EB50`. Each segment is 0xEC (236) bytes. Maximum 0x50 (80) segments total.

**Addresses:**

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x43EDA9` | `CMP EDX,0x50` | Tire track pool: free slot search bound |
| `0x43EDD0` | `CMP EDX,0x50` | Tire track pool: free slot search (path 2) |
| `0x43EDF8` | `CMP EDX,0x50` | Tire track pool: free slot search (path 3) |
| `0x43F047` | `CMP ECX,0x50` | Tire track pool: segment limit check |
| `0x43F075` | `CMP ECX,0x50` | Tire track pool: segment limit check |
| `0x43F09D` | `CMP ECX,0x50` | Tire track pool: segment limit check |

The track segment pool starts at `DAT_004C375C + 0xD8` with stride 0xEC. The rolling write index is at `DAT_004C3758`.

**Safe patch range:** The pool is in BSS at a computed offset from `DAT_004C375C`. Increasing from 80 to 128 segments requires ~5.6 KB additional BSS space and patching 6 CMP instructions.

**Risk: EASY** -- Simple constant patches. The pool is contiguous and the limit is checked consistently.

---

### VE-3. Particle Active Sprite Limit (50 per viewport)

The active particle sprite tracking array at `DAT_004A6370` has 0x32 (50) byte entries per viewport.

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x429720+` | `param_4 * 0x32` | Particle active flag array stride |
| `0x42961F` | `CMP EAX,0x32` | Active sprite limit check |
| `0x429749` | `CMP ECX,0x32` | Active sprite limit check |
| `0x429A80` | `CMP EAX,0x32` | SpawnVehicleSmokeVariant: active slot search |
| `0x429D59` | `CMP EAX,0x32` | Smoke variant: active slot search |
| `0x42A020` | `CMP EAX,0x32` | Smoke variant: active slot search |
| `0x42A314` | `CMP EAX,0x32` | Smoke variant: active slot search |
| `0x42A723` | `CMP ECX,0x32` | Smoke variant: active slot search |

**Safe patch range:** Increasing from 50 to 64 or 80 visible sprites requires expanding the active flag array and patching all 7+ CMP instructions. Must remain <= the total particle slot count (VE-1).

**Risk: EASY** -- All simple immediates, but coupled to VE-1.

---

## ASSET LOADING LIMITS

### AS-1. Loading Screen Image Size (640x480 hardcoded)

*Fully documented in `hardcoded-resolution-audit.md` items #7 and #8.*

Loading screen TGA images are assumed to be exactly 640x480. The copy loop at `0x42CA00` hardcodes:
- Width stride: `0x280` (640)
- Total pixel count: `0x4B000` (307200 = 640 x 480)

| Address | Value | What |
|---------|-------|------|
| `0x42CA00+` | `0x280` | Loading screen width stride |
| `0x42CA00+` | `0x4B000` | Loading screen total pixel limit |

**Risk: EASY** -- Two constants in one function. Requires matching TGA asset dimensions.

---

### AS-2. Loading Screen Count (2 legal screens)

The legal/loading screen display at `0x42C8E0` (ShowLegalScreens) hardcodes exactly 2 legal screen images: `legal1.tga` and `legal2.tga` from `LEGALS.ZIP`.

| Address | What |
|---------|------|
| `0x4672F4` (string) | `"legal1.tga"` |
| `0x4672E8` (string) | `"legal2.tga"` |
| `0x467300` (string) | `"LEGALS.ZIP"` |

The function displays legal1, waits up to 5 seconds (or keypress after 400ms), then displays legal2 with the same timing. Adding more screens requires code changes to the function.

**Risk: MODERATE** -- The function structure is sequential (not a loop), so adding screens requires restructuring or injecting a loop.

---

### AS-3. Frontend Screen Function Table (30 entries)

The frontend screen table at `g_frontendScreenFnTable` (`0x4655C4`) holds up to 30 function pointers for menu screens.

| Address | What |
|---------|------|
| `0x4655C4` (data) | Screen function pointer table (30 x 4 = 120 bytes) |

Used screens: indices 0-0x17 (24 of 30 slots used). 6 slots remain available for custom screens.

**Risk: EASY** -- 6 unused slots available. Adding screens beyond 30 requires expanding the table.

---

## PHYSICS LIMITS

*Tick-rate-dependent physics constants are fully documented in `tick-rate-dependent-constants.md`. Only structural limits are listed here.*

### PH-1. Fixed-Point Depth Range (0x1000 = 4096)

The physics and rendering systems use 12-bit fixed-point (>> 12) for many calculations. The depth range constant 0x1000 appears in:

| Address | Instruction | Context |
|---------|-------------|---------|
| `0x43E7E6` | `MOV [0x467368],0x1000` | Projection setup: depth range |
| `0x440A67` | `MOV [0x4C37D8],0x1000` | Sound: engine crossfade target |
| `0x440AC8` | `MOV [0x4C37D8],0x1000` | Sound: engine crossfade target |
| `0x440C24` | `MOV [0x4C38B8],0x1000` | Sound: crossfade threshold |
| `0x40BF09` | `MOV [0x4BF500],0x1000` | Actor marker intensity max |
| `0x43D812` | `MOV [0x4BF500],0x1000` | Actor marker intensity reset |

The 0x1000 value serves as both a depth bucket count AND a fixed-point unit (1.0 in 12-bit). Context determines which meaning applies at each site.

**Risk: N/A for most** -- These are fixed-point representations, not arbitrary limits. Only the depth sort bucket count (DL-1) is meaningfully patchable.

---

### PH-2. Gravity Constants (difficulty-dependent)

| Address | Value | Difficulty |
|---------|-------|------------|
| `0x42F29F` | `0x800` (2048) | Hard |
| `0x42F354` | `0x5DC` (1500) | Easy |
| `0x42F360` | `0x76C` (1900) | Normal |

Written to global `DAT_00467380` (gGravityConstant) at race init.

**Safe patch range:** Values are applied per physics tick. Halving makes vehicles floatier; doubling makes them heavier. Range 0x200-0x1000 is reasonable.

**Risk: EASY** -- Three MOV immediates in one function.

---

## SUMMARY TABLE

| ID | Limit | Current | Category | Locations | Risk | Notes |
|----|-------|---------|----------|-----------|------|-------|
| GL-1 | Race actors | 6 | Gameplay | ~30+ | HARD | Static BSS array, stride 0x388 |
| GL-2 | Traffic actors | ~8 | Gameplay | ~5 | HARD | Contiguous with race actors |
| GL-3 | Cup races/series | 4-12 | Gameplay | 1 table | MODERATE | Data table at 0x4640A4 |
| GL-4 | Track pool | 18 | Gameplay | 3 tables | MODERATE | Data-driven, needs new assets |
| GL-5 | Car models | data-driven | Gameplay | 0 (data) | EASY | Controlled by carparam.dat |
| DL-1 | Depth sort buckets | 4096 | Display | 8+ | MODERATE | All write 0x1000 to 0x467368 |
| DL-2 | Render nodes | 512 | Display | 2 | EASY | Single function |
| DL-3 | Sort node pool | 64 KB | Display | 1 | EASY | Single allocation |
| AL-1 | Sound buffer slots | 44+44 | Audio | 6+ (EXE) + DLL | HARD | Cross-binary patch |
| AL-2 | Channels/actor | 3 | Audio | pervasive | HARD | Structural multiplier |
| VE-1 | Particles/viewport | 100 | Visual FX | 10+ | MODERATE | BSS array, many CMPs |
| VE-2 | Tire track segments | 80 | Visual FX | 6 | EASY | Simple constant patches |
| VE-3 | Particle sprites | 50 | Visual FX | 7+ | EASY | Coupled to VE-1 |
| AS-1 | Loading screen size | 640x480 | Asset Loading | 2 | EASY | Needs matching TGA |
| AS-2 | Legal screen count | 2 | Asset Loading | code structure | MODERATE | Sequential, not looped |
| AS-3 | Frontend screens | 30 | Asset Loading | 1 table | EASY | 6 unused slots |
| PH-1 | Depth range | 0x1000 | Physics | many | N/A | Fixed-point unit |
| PH-2 | Gravity | 1500-2048 | Physics | 3 | EASY | Difficulty-dependent |

---

## RECOMMENDED MODDING PRIORITIES

### Tier 1: Quick Wins (EASY risk, high impact)
1. **PH-2: Gravity** -- Adjust vehicle weight feel. 3 patches in one function.
2. **VE-2: Tire track segments** -- More detailed tire marks. 6 patches.
3. **DL-2/DL-3: Render node/sort pools** -- Prevent polygon dropout in complex scenes. 2-3 patches.
4. **GL-5: Car models** -- Data-only, no EXE changes needed.

### Tier 2: Moderate Effort (MODERATE risk, high impact)
1. **DL-1: Depth sort buckets** -- Better Z-precision. 8+ patches but all the same pattern.
2. **VE-1/VE-3: Particle limits** -- Denser smoke/dust effects. ~17 patches total.
3. **GL-3/GL-4: Cup schedules and track pool** -- Custom championships. Data table patches.

### Tier 3: Major Effort (HARD risk)
1. **GL-1/GL-2: Actor count** -- More racers on track. Requires BSS relocation or heap allocation rewrite.
2. **AL-1: Sound buffer slots** -- More audio channels. Cross-binary M2DX.dll patch.
3. **AL-2: Sound channels per actor** -- Richer per-vehicle audio. Deep structural change.

---

## KEY GLOBALS REFERENCE (New, not in other docs)

| Global | Address | Type | Purpose |
|--------|---------|------|---------|
| gActorArray base | `0x4AB108` | struct[14] | Actor array (6 race + ~8 traffic), stride 0x388 |
| gActorTypeFlags | `0x4AADF4` | dword[6] | Per-race-actor type flag (1=player, etc.) |
| gRaceOrderTable | `0x4AE278` | byte[6] | Race position -> actor index mapping |
| gTotalActorCount | `0x4AAF00` | dword | Runtime total actor count |
| gDepthBucketRange | `0x467368` | dword | Depth sort bucket count (usually 0x1000) |
| gDepthBucketArray | `0x4BF6C8` | ptr | Pointer to depth sort bucket array |
| gSortNodePool | `0x4BF6C4` | ptr | Pointer to sort node allocation pool |
| gRenderNodePool | `0x4AF26C` | ptr | Pointer to render node linked list pool |
| gTireTrackWriteIdx | `0x4C3758` | dword | Next tire track segment to allocate |
| gTireTrackPoolBase | `0x4C375C` | ptr | Tire track segment pool base |
| gParticlePool | `0x4A318F` | struct[200] | Particle pool (100 per viewport, stride 0x40) |
| gParticleActiveFlags | `0x4A6370` | byte[100] | Per-particle active flags (50 per viewport) |
| gCupScheduleTable | `0x4640A4` | byte[] | Cup race track schedules |
| gFrontendScreenTable | `0x4655C4` | fptr[30] | Frontend screen function pointers |
| gGravityConstant | `0x467380` | dword | Runtime gravity (set per difficulty) |
