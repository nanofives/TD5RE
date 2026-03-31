# Archive System & Vehicle Asset Loading Pipeline

> Deep-dive reverse engineering of the ZIP/VFS archive system and the full asset loading pipeline
> from ZIP open through texture upload to D3D surfaces.

---

## Table of Contents

1. [M2DX.DLL ArchiveProviderManager (VFS)](#1-m2dxdll-archiveprovidermanager-vfs)
2. [EXE ZIP System](#2-exe-zip-system)
3. [Master Race Init: InitializeRaceSession](#3-master-race-init-initializeracesession)
4. [Track Runtime Data Loading](#4-track-runtime-data-loading)
5. [Vehicle Asset Loading](#5-vehicle-asset-loading)
6. [Texture Loading Pipeline](#6-texture-loading-pipeline)
7. [Track Texture Cache Builder](#7-track-texture-cache-builder)
8. [TGA Decoder](#8-tga-decoder)
9. [Memory Management](#9-memory-management)
10. [ZIP File Inventory](#10-zip-file-inventory)

---

## 1. M2DX.DLL ArchiveProviderManager (VFS)

The M2DX engine includes a generic virtual filesystem (VFS) layer called `ArchiveProviderManager`.
This is a ref-counted COM-like object with a vtable, designed to abstract file access through
pluggable "provider" DLLs. **In practice, TD5 does NOT use this VFS for game assets** -- the EXE
has its own direct ZIP reader. The VFS is used only by M2DX's optional CD-audio backend and
similar subsystem init paths.

### 1.1 Class Layout (0x24 bytes = 36 bytes)

```c
struct ArchiveProviderManager {  // size = 0x24 (36 bytes)
    /* 0x00 */ void**   vtable;           // -> vtable at 0x1001d2a4
    /* 0x04 */ int      refCount;         // ref count, init = 1
    /* 0x08 */ int      initParam;        // copy of param_1 from constructor
    /* 0x0C */ void*    activeProvider;    // current provider object (COM-like, has own vtable)
    /* 0x10 */ char     busyFlag;         // set during OpenPath, prevents re-entry
    /* 0x14 */ char     workPath[???];    // inline path buffer (used by OpenPath/ClosePath)
    /* 0x18 */ void*    providerArray;    // heap-allocated array of provider records
    /* 0x1C */ void*    providerArrayEnd; // pointer past last valid record
    /* 0x20 */ void*    providerArrayCap; // pointer past allocated capacity
};
```

### 1.2 Provider Record (0x210 bytes = 528 bytes per record)

Each registered provider is a 0x210-byte record:

```c
struct ArchiveProviderRecord {  // size = 0x210 (528 bytes)
    /* 0x000 */ char     data[0x208];      // provider-specific opaque data (path, config, etc.)
    /* 0x208 */ HMODULE  hModule;          // DLL handle (FreeLibrary'd on destroy)
    /* 0x20C */ code*    createCallback;   // factory function: createCallback(hModule, manager) -> provider*
};
```

### 1.3 Vtable at 0x1001d2a4 (12 entries)

| Slot | Address      | Name / Purpose |
|------|-------------|----------------|
| 0    | 0x10013cd0  | `QueryInterface` -- COM-style QI; returns self for type 1 or 0x1001, increments refcount |
| 1    | 0x10013d00  | `AddRef` -- increments refCount (offset +4) |
| 2    | 0x10013d10  | `Release` -- decrements refCount; if zero, calls destructor + free |
| 3    | 0x100131a0  | `OpenPath` -- main entry: normalizes path, dispatches to providers |
| 4    | 0x10013250  | `ClosePath` -- mirror of OpenPath but only runs if busyFlag is already set |
| 5    | 0x100132f0  | `TryOpenWithProviders` -- iterates provider array, calls each until one succeeds |
| 6    | 0x10013360  | `stub_returnZero_1` -- returns 0 (virtual placeholder) |
| 7    | 0x10013370  | `stub_returnZero_2` -- returns 0 (virtual placeholder) |
| 8    | 0x10013380  | `SetWorkingDirectory` -- (not fully analyzed, in same code block) |
| 9    | 0x10013390  | `EnsureTrailingBackslash` -- if path empty, fills with CWD; appends `\` if missing |
| 10   | 0x100133f0  | `FormatProviderPath` -- formats provider-local path with prefix |
| 11   | 0x10013900  | `DispatchToProviders` -- internal dispatch after path normalization |

### 1.4 Lifecycle

**CreateArchiveProviderManager** (0x10013060):
```
1. operator new(0x24) -- allocate 36-byte object
2. Set vtable -> 0x1001d2a4
3. refCount = 1
4. busyFlag = 0, activeProvider = NULL
5. providerArray = providerArrayEnd = providerArrayCap = NULL
6. Call vtable[3] (init callback) with (param_2, param_3)
7. If init fails, call vtable[2] (Release) and return NULL
```

**DestroyArchiveProviderManager** (0x100130d0):
```
1. Iterate providerArray -> providerArrayEnd (stride 0x210)
2. For each: if hModule != NULL, FreeLibrary(hModule)
3. Reset providerArrayEnd = providerArray (logical clear)
4. If activeProvider != NULL, call activeProvider->vtable[2] (Release)
5. Free providerArray heap block
6. Zero all array pointers
```

**ReleaseArchiveProviderManager** (0x10013d10):
```
1. refCount--
2. If refCount == 0: call DestroyArchiveProviderManager, then free(this)
3. Return new refCount
```

### 1.5 OpenPath Flow (0x100131a0)

```
1. If busyFlag != 0: clear busyFlag, return 0 (re-entry guard)
2. Copy input path to local 260-byte buffer
3. Call vtable[9] (EnsureTrailingBackslash) on local buffer
4. Call vtable[10] (FormatProviderPath) to build provider-qualified path
5. Call vtable[11] (DispatchToProviders) which calls TryOpenWithProviders
6. Set busyFlag = result
7. Return result
```

### 1.6 TryOpenWithProviders (0x100132f0)

```
1. Walk providerArray from offset 0x18 to offset 0x1C (stride 0x210)
2. For each record where createCallback (offset +0x20C) != NULL:
   a. Call createCallback(hModule, this) -> providerInstance
   b. If providerInstance != NULL:
      - Call providerInstance->vtable[0](param_1, param_2)
      - If returns true: set success = true, break
      - Call providerInstance->vtable[2]() (Release)
3. Return success
```

### 1.7 Provider Table Management

| Function | Address | Purpose |
|----------|---------|---------|
| `CopyArchiveProviderRecords` | 0x10013c50 | Copies provider records (0x84 dwords = 0x210 bytes each) from src range to dst |
| `CopyArchiveProviderRecordTemplate` | 0x10013c90 | Fills N consecutive records from a single template |
| `ClearArchiveProviderTable` | 0x10013be0 | Frees the provider array and zeros all 3 pointers |
| `GetArchiveProviderCount` | 0x10013c10 | Returns `(end - begin) / 0x210`, or 0 if begin is NULL |

---

## 2. EXE ZIP System

The EXE implements its own ZIP reader **completely independent of the M2DX VFS**. This is the
system actually used for all game asset loading. It operates at the C stdio level (fopen/fread/fseek).

### 2.1 Core Architecture

The ZIP system consists of 4 key functions:

| Function | Address | Purpose |
|----------|---------|---------|
| `GetArchiveEntrySize` | 0x4409b0 | Get uncompressed size of a file inside a ZIP |
| `ReadArchiveEntry` | 0x440790 | Read + decompress a file from a ZIP into memory |
| `ParseZipCentralDirectory` | 0x43fc80 | Parse ZIP central directory to find an entry |
| `DecompressZipEntry` | 0x4405b0 | Decompress a single ZIP local file entry |

Plus the DEFLATE engine:

| Function | Address | Purpose |
|----------|---------|---------|
| `InflateDecompress` | 0x447fe2 | Main INFLATE decompressor (method 8) |
| `InflateProcessDynamicHuffmanBlock` | (called by above) | Dynamic Huffman block handler |
| `InflateProcessFixedHuffmanBlock` | (called by above) | Fixed Huffman block handler |
| `InflateProcessStoredBlock` | (called by above) | Stored (uncompressed) block handler |

### 2.2 Key Globals

| Address | Type | Purpose |
|---------|------|---------|
| `DAT_004c3760` | `int*` | 64KB I/O buffer (malloc'd on first use, freed after each operation) |
| `DAT_004cf97c` | `FILE*` | Currently open ZIP file handle |
| `DAT_004cf984` | `uint` | Current file position / central directory offset |
| `DAT_004cf978` | `uint` | Remaining bytes in central directory to process |
| `DAT_004cf980` | `uint` | Bytes read in current chunk |
| `DAT_0047b1d4` | `uint` | Read cursor within 64KB buffer (0x10000 = buffer exhausted, triggers refill) |
| `DAT_004cf988` | `uint` | Local file header offset (result of central directory search) |
| `DAT_004c3764` | `char*` | Output destination pointer for decompression |
| `DAT_0047b1dc` | `uint` | Expected CRC32 from ZIP header |
| `DAT_0047b1d8` | `uint` | Running CRC32 of decompressed data |
| `DAT_0047b1ec` | `int` | Flag: if nonzero, use streaming fwrite mode instead of memory buffer |
| `DAT_00475160` | `uint[256]` | CRC32 lookup table |

### 2.3 File Access: Transparent ZIP-or-Loose

Both `GetArchiveEntrySize` and `ReadArchiveEntry` implement **transparent fallback**:

```
GetArchiveEntrySize(entryName, zipPath):
    1. Try fopen(entryName, "rb")  // try as loose file on disk
    2. If success: fseek(END), ftell() -> return file size
    3. If fail: strip path prefix from entryName (find last \ or /)
    4. Call ParseZipCentralDirectory(basename, zipPath)
    5. Return compressed_size from central directory entry

ReadArchiveEntry(entryName, zipPath, destBuf, maxSize):
    1. Try fopen(entryName, "rb")  // try as loose file
    2. If success: fread(destBuf, 1, maxSize, file) -> return bytes read
    3. If fail: strip path prefix, call ParseZipCentralDirectory(basename, zipPath)
    4. If found: fopen(zipPath), fseek to DAT_004cf988 (local header offset)
    5. Call DecompressZipEntry() -> return uncompressed size
```

This means **any file inside a ZIP can be overridden by placing a loose file with the same name
on disk**. The game checks the filesystem first, falling back to the ZIP only if fopen fails.

### 2.4 ParseZipCentralDirectory (0x43fc80)

Full ZIP central directory parser. Searches for a named entry within a ZIP archive.

**Algorithm:**
```
1. Allocate 64KB I/O buffer if not already allocated
2. Open ZIP file (fopen)
3. Seek to EOF-22 bytes, read 22 bytes (End of Central Directory Record)
4. Check for EOCD signature 0x06054b50
   - If not found at expected position: read last 64KB of file,
     scan backwards byte-by-byte for signature 0x06054b50
5. Extract from EOCD:
   - total_entries = *(uint16*)(eocd + 8)   // total entries in central dir
   - cd_offset    = *(uint32*)(eocd + 16)   // offset of central directory
   - cd_size      = *(uint32*)(eocd + 12)   // size of central directory
6. For each entry (total_entries times):
   a. Read 4-byte signature, verify == 0x02014b50 (central dir entry)
   b. Skip 4 bytes (version made by, version needed)
   c. Read 4 bytes: general_flags (bit 0 = encrypted, bits 16-23 = external attrs)
   d. Filter: skip if encrypted (bit 0 set) or external_attrs not 0x00 or 0x80
   e. Skip 12 bytes (CRC, compressed/uncompressed sizes already read elsewhere)
   f. Read 4 bytes: compressed_size -> stored in local_120
   g. Read 2 bytes: filename_length
   h. Read 2 bytes: extra_field_length
   i. Read 2 bytes: comment_length
   j. Skip 8 bytes (disk number, internal attrs, external attrs)
   k. Read 4 bytes: local_header_offset -> stored in DAT_004cf988
   l. If compressed_size == 0: skip (filename_len + extra_len + comment_len) bytes
   m. If compressed_size != 0:
      - Read filename bytes into local buffer
      - Strip directory prefix from filename (find last / or \)
      - Case-insensitive compare basename vs search target
      - If match: set found = true, store offset, break
7. Close file, free buffer
8. Return compressed_size if found, 0 if not
```

**Key details:**
- Uses a **streaming 64KB buffer** with refill-on-exhaust pattern (DAT_0047b1d4 == 0x10000 triggers refill)
- Byte-by-byte field reading with manual shift-accumulate (no struct cast)
- Skips encrypted entries (bit 0 of general flags)
- Skips entries with external attributes not 0x00 (stored) or 0x80 (normal)
- **Case-insensitive** filename matching (stricmp)
- Strips directory paths from ZIP entry names before comparison

### 2.5 DecompressZipEntry (0x4405b0)

Decompresses a single file from a ZIP local file header.

```
1. Read 30-byte local file header
2. Verify signature == 0x04034b50
3. Extract compression_method (offset 0x08, low byte)
4. Extract expected_crc32 (offset 0x0E)
5. Extract uncompressed_size (offset 0x16, 4 bytes at 0x16)
6. Skip filename + extra field (filename_len at offset 0x1A, extra_len at offset 0x1C)
7. Switch on compression_method:
   - method 0 (STORED):
     * If not streaming: fread directly to output buffer, compute CRC32
     * If streaming (DAT_0047b1ec != 0): read in 32KB chunks, CRC32 each, fwrite to output file
     * Verify CRC32 matches expected
   - method 8 (DEFLATE):
     * Call InflateDecompress() which handles dynamic/fixed/stored Huffman blocks
     * Returns uncompressed size
8. Free 64KB I/O buffer
9. Return uncompressed_size on success, 0 on failure
```

**CRC32 verification** is performed for STORED entries. The CRC32 table is at `0x00475160`.

### 2.6 InflateDecompress (0x447fe2)

Standard DEFLATE implementation:
```
1. Initialize: CRC=0, bit buffer=0, bit count=0
2. Refill input buffer (64KB chunks from ZIP via fread)
3. Read 3 header bits: BFINAL (1 bit), BTYPE (2 bits)
4. Switch on BTYPE:
   - 0: InflateProcessStoredBlock (raw copy)
   - 1: InflateProcessFixedHuffmanBlock (static Huffman tables)
   - 2: InflateProcessDynamicHuffmanBlock (inline Huffman tables)
5. Loop until BFINAL == 1
6. Flush remaining output, compute final CRC32
```

### 2.7 DecompressTrackDataStream (0x43fbc0)

A separate byte-stream reader used by `ParseZipCentralDirectory` for reading multi-byte fields.
Reads N bytes from the 64KB buffer, assembling them LSB-first into a uint. Triggers buffer refill
when the read cursor hits 0x10000.

---

## 3. Master Race Init: InitializeRaceSession

**Address:** 0x42AA10

This is the master orchestrator that loads all race assets. Called when transitioning from the
frontend to a race. Full sequence:

```
 1. Set random seeds for AI variation
 2. Load + display loading screen image from LOADING.ZIP (load%02d.tga)
 3. ResetGameHeap() -- destroy and recreate the game heap (24MB)
 4. Initialize race slot state table (6 slots, player/AI/disabled)
 5. Configure game mode flags (time trial, wanted, drag, circuit, etc.)
 6. Set track index (DAT_004aad90) and direction (gReverseTrackDirection)
 7. Configure viewport layout (fullscreen vs split-screen)
 8. Disable traffic in benchmark/network/split-screen modes
 9. LoadTrackRuntimeData(trackIndex)        -- STRIP/CHECKPT/LEFT/RIGHT/TRAFFIC/LEVELINF
10. Configure circuit/encounter/traffic flags from LEVELINF.DAT
11. Map car selection indices to car type indices
12. BindTrackStripRuntimePointers()          -- parse STRIP.DAT
13. ApplyTrackStripAttributeOverrides()
14. Load MODELS.DAT from level%03d.zip       -- track 3D mesh data
15. LoadTrackTextureSet(trackIndex)           -- TEXTURES.DAT + static.hed
16. ParseModelsDat()                          -- parse track mesh, get span ring length
17. Load SKY.PRR from static.zip             -- sky dome mesh
18. LoadRaceVehicleAssets()                   -- himodel.dat + carparam.dat + sounds + traffic
19. InitializeRaceVehicleRuntime()
20. InitializeRaceActorRuntime()
21. Initialize shadows, wheels, sprites
22. Position all actors on track (InitializeActorTrackPose)
23. Open DXInput recording/playback
24. Load ambient sounds
25. Initialize particles, smoke, tire tracks, weather
26. InitializeTrafficActorsFromQueue()
27. Configure force feedback controllers
28. Initialize cameras
29. DXInput::SetConfiguration()
30. Start CD audio
31. InitializeWantedHudOverlays()
32. InitializeRaceHudFontGlyphAtlas()
33. InitializeRaceRenderState()              -- create D3D viewport, render targets
34. Configure fog from LEVELINF.DAT
35. Select display mode, go fullscreen if needed
36. Set render dimensions (gRenderWidth, gRenderHeight)
37. ConfigureProjectionForViewport()
38. QueryRaceTextureCapacity()
39. InitializeRaceOverlayResources()
40. InitializeRaceHudLayout()
41. RemapCheckpointOrderForTrackDirection()  -- if reverse
42. LoadRaceTexturePages()                   -- ALL texture uploads (track, sky, cars, traffic, env)
43. InitializePauseMenuOverlayLayout()
44. Reset frame counter
```

---

## 4. Track Runtime Data Loading

### LoadTrackRuntimeData (0x42fb90)

Loads 6 files from `level%03d.zip` (where %03d = track index):

| File | Direction Variants | Global Storage | Purpose |
|------|-------------------|----------------|---------|
| `STRIP.DAT` / `STRIPB.DAT` | Yes (gReverseTrackDirection selects) | `DAT_004aed90` | Track strip/span geometry |
| `CHECKPT.NUM` | No | `DAT_004aedb0` (inline, 0x60 bytes) | Checkpoint span indices |
| `LEFT.TRK` / `LEFTB.TRK` | Yes | `DAT_004aed94` | Left boundary spline |
| `RIGHT.TRK` / `RIGHTB.TRK` | Yes | `DAT_004aee1c` | Right boundary spline |
| `TRAFFIC.BUS` / `TRAFFICB.BUS` | Yes | `DAT_004aed8c` | Traffic spawn queue |
| `LEVELINF.DAT` | No | `gTrackEnvironmentConfig` | Environment config (fog, circuit flag, traffic enable, etc.) |

**Direction-dependent file selection** uses a pointer table at 0x4673B8:
```
Forward: LEFT.TRK,  RIGHT.TRK,  TRAFFIC.BUS,  STRIP.DAT
Reverse: LEFTB.TRK, RIGHTB.TRK, TRAFFICB.BUS, STRIPB.DAT
```
Selected by: `table[gReverseTrackDirection * 4]`

After loading, the function copies 24 bytes of per-track metadata from a static table and
sets up trackside camera profile pointers.

---

## 5. Vehicle Asset Loading

### LoadRaceVehicleAssets (0x443280)

Loads all vehicle models, physics parameters, sounds, and traffic models.

**Phase 1: Calculate buffer sizes for racer models**
```
for each racer slot (0..5, or 0..1 in time trial):
    size = GetArchiveEntrySize("himodel.dat", gCarZipPathTable[gSlotCarTypeIndex[i]])
    size = ALIGN_32(size)   // round up to 32-byte boundary
    offsets[i] = running_total
    sizes[i] = size
    running_total += size
```
If `DAT_004c3d44 == 2` (cop car duplicate mode), allocates an extra copy of slot 0's model.

**Phase 2: Single heap allocation**
```
base = HeapAllocTracked(total_size + 0x1F)
base = ALIGN_32(base)   // 32-byte align the base pointer
// Rebase all offsets to absolute addresses
for each slot: offsets[i] += base
```

**Phase 3: Load per-vehicle data**
```
for each racer slot:
    1. ReadArchiveEntry("himodel.dat", carZipPath, offsets[i], sizes[i])
    2. ReadArchiveEntry("carparam.dat", carZipPath, tempBuf, 0x10C)
    3. Copy first 0x8C bytes of carparam -> gVehicleTuningTable[i]    (0x23 dwords = 0x8C bytes)
    4. Copy remaining 0x80 bytes -> gVehiclePhysicsTable[i]           (0x20 dwords = 0x80 bytes)
    5. LoadVehicleSoundBank(carZipPath, slot, isLocal)
    6. Patch UV coordinates:
       - Scale U coords: u = u * 0.5 + (slot & 1) * 0.5
         (this tiles two car skins into one 2x texture page)
    7. Set texture page index: model_header[+2] = slot/2 + chassis_texpage
    8. PatchModelUVCoordsForTrackLighting(model)
    9. PrepareMeshResource(model)
```

**carparam.dat layout (0x10C = 268 bytes):**
```
Offset 0x00..0x8B: Vehicle tuning table (0x23 dwords = 35 floats/ints)
Offset 0x8C..0x10B: Vehicle physics table (0x20 dwords = 32 floats/ints)
```

**Phase 4: Traffic models (if enabled)**
```
for each traffic type (0..5):
    1. size = GetArchiveEntrySize(sprintf("model%d.prr", DAT_004aad90), "traffic.zip")
    2. Allocate all traffic model buffers in one HeapAllocTracked call
    3. ReadArchiveEntry("model%d.prr", "traffic.zip", ...)
    4. Copy default tuning params from gVehicleTuningTable[0] as template
    5. Find texture entry via FindArchiveEntryByName(sprintf("TRAF%d", ...))
    6. Set texture page from static.hed entry
    7. PatchModelUVCoordsForTrackLighting + PrepareMeshResource
```

### UV Tiling Scheme

Two cars share each texture page. The U coordinate is halved and offset:
- Even slots (0, 2, 4): `u = u * 0.5` (left half)
- Odd slots (1, 3, 5): `u = u * 0.5 + 0.5` (right half)

This is why `LoadRaceTexturePages` composites two CARSKIN TGA files side-by-side.

---

## 6. Texture Loading Pipeline

### LoadTrackTextureSet (0x442670)

```
1. InitializeTrackStripMetadata(trackIndex)
2. Load TEXTURES.DAT from level%03d.zip -> heap buffer
3. Load static.hed from static.zip -> heap buffer
4. Parse static.hed header:
   - gTrackTextureCount = header[0] - 4  (or -4 stored in DAT_004c3cf4 for mode 2)
   - gStaticHedEntryCount = header[1]
   - gStaticHedEntryArray = &header[2]       // array of 0x40-byte entries
   - gStaticHedTextureData = entries + entryCount * 0x10 dwords
5. Rebase internal pointers: for each entry, entry[+0x40] += 0x400
6. BuildTrackTextureCache(textureCount, textureData, texturesDAT)
```

### LoadStaticTrackTextureHeader (0x442560)

A lighter version that just reads static.hed to determine texture counts and dimensions:
```
1. Read static.hed from static.zip
2. For each texture page (0..textureCount):
   - Read width (entry[+0x08]) and height (entry[+0x0C])
   - If DAT_004c3d04 == 0: use min(width, height) for both dimensions
   - Store in Texture_exref at stride 0x30
   - Accumulate total pixel count in Set_exref+4
3. Log: "Static Textures: %d using %d Pixels"
```

### static.hed Entry Format (0x40 = 64 bytes per entry)

```c
struct StaticHedEntry {
    /* 0x00 */ char  name[32];          // e.g. "SKY", "CAR0", "wheels", "TRAF0"
    /* 0x20 */ int   unknown_20;        //
    /* 0x24 */ int   width_or_dim1;     // texture width
    /* 0x28 */ int   height_or_dim2;    // texture height
    /* 0x2C */ int   format_info;       // pixel format descriptor
    /* 0x30 */ ...
    /* 0x3C */ int   texturePageIndex;  // index into D3D texture table (offset +0x400)
};
```

### LoadRaceTexturePages (0x442770)

The master texture upload function. Allocates two temp buffers (512KB + 256KB) and processes
all texture types in sequence:

**Step 1: Track texture pages (tpage%d.dat from static.zip)**
```
for i in 0..gTrackTextureCount:
    if entry has data:
        ReadArchiveEntry(sprintf("tpage%d.dat", i), "static.zip", buf512k, 0x80000)
        ResampleTexturePageToEntryDimensions(buf512k, i)
        UploadRaceTexturePage(buf512k, i, formatMode, isEnvMap)
```

**Step 2: Sky texture (FORWSKY.TGA or BACKSKY.TGA from level%03d.zip)**
```
ReadArchiveEntry(skyName, levelZip, buf512k, 0x80000)
DecodeArchiveImageToRgb24(buf512k, buf512k + 0x40000)
Find "SKY" entry in static.hed
ResampleTexturePageToEntryDimensions(decoded, skyEntry.pageIndex - 0x400)
UploadRaceTexturePage(decoded, skyEntry.pageIndex - 0x400, 0, 0)
```

**Step 3: Car skin textures (pairs of CARSKIN%d.TGA)**
```
for slot_pair in [0, 2, 4]:     // 3 pairs for 6 racers
    Find "CAR%d" entry in static.hed
    // Load left half (even slot)
    ReadArchiveEntry(sprintf("CARSKIN%d.TGA", slot_pair), carZip[even], buf512k, 0x40000)
    DecodeArchiveImageToRgb24 -> copy left 32 columns of each row to buf256k left half
    // Load right half (odd slot)
    ReadArchiveEntry(sprintf("CARSKIN%d.TGA", slot_pair), carZip[odd], buf512k, 0x40000)
    DecodeArchiveImageToRgb24 -> copy to buf256k right half
    // Upload combined 2-car texture
    ResampleTexturePageToEntryDimensions + UploadRaceTexturePage
```

The skin compositing copies 32 pixels (0x20) per row from each 256x256 TGA, interleaving
them into a single 512-wide (or 256-wide with half-U) texture page. Each row is 0x180 bytes
apart in the destination (stride for 128-wide RGB24 = 384 bytes).

**Step 4: Wheel hub textures (CARHUB%d.TGA)**
```
Load tpage%d.dat for "wheels" entry as base
for each slot (0..5):
    ReadArchiveEntry(sprintf("CARHUB%d.TGA", slot), carZip[slot], buf512k, 0x40000)
    DecodeArchiveImageToRgb24
    Convert RGB24 -> ARGB32 (alpha = 0xFF if any color, 0x00 if black)
    Place 64x64 hub into 256x256 grid: position = (slot%4, slot/4) * 64
Upload combined wheel texture page
```

**Step 5: Traffic textures (if enabled)**
```
for each traffic type (0..5):
    Find "TRAF%d" entry in static.hed
    LoadTrafficVehicleSkinTexture(trackIndex, type, buf512k, 0x80000)
    ResampleTexturePageToEntryDimensions + UploadRaceTexturePage
```

**Step 6: Cleanup and environment**
```
free(buf256k), free(buf512k)
ResetTexturePageCacheState()
LoadEnvironmentTexturePages()
PreloadLevelTexturePages()
```

### LoadTrafficVehicleSkinTexture (0x4431c0)

Simple wrapper:
```
1. malloc(param_4) for temp buffer
2. ReadArchiveEntry(sprintf("skin%d.tga", type), "traffic.zip", temp, size)
3. DecodeArchiveImageToRgb24(temp, output)
4. free(temp)
```

### LoadEnvironmentTexturePages (0x42f990)

Loads environment maps from `environs.zip`:
```
Mode 1 (DAT_004c3d44 == 1): Direct upload
    for i in 0..*DAT_004aee10:
        ReadArchiveEntry(entryName, "environs.zip", buf, 0x20000)
        NoOpHookStub()  // placeholder for env processing

Mode 2 (DAT_004c3d44 == 2): TGA decode + upload
    for i in 0..*DAT_004aee10:
        Find "ENV%d" entry in static.hed
        ReadArchiveEntry(entryName, "environs.zip", buf, 0x20000)
        DecodeArchiveImageToRgb24(buf, buf + 0x10000)
        ResampleTexturePageToEntryDimensions
        UploadRaceTexturePage(decoded, entry.pageIndex - 0x400, 0, 1)  // isEnvMap=1
```

---

## 7. Track Texture Cache Builder

### BuildTrackTextureCacheImpl (0x40b1d0)

Builds the D3D texture cache from TEXTURES.DAT (paletted track textures).

**Cache structure allocation:**
```
total = numStaticEntries * 4      // pointer array
      + numTexDatEntries * 9      // per-entry metadata (8 bytes + 1 byte flag)
      + 0x98C                     // fixed overhead (LRU list, etc.)

DAT_0048dc3c = HeapAllocTracked(total)
```

**Cache layout (DAT_0048dc3c):**
```
+0x00: pointer to static entry pointer array (offset +0x0C from base)
+0x04: 0x400 (base texture index offset)
+0x08: numStaticEntries (count)
+0x0C: [numStaticEntries * 4 bytes] -- pointer array to static entry data

DAT_0048dc40 = base + numStaticEntries + 3 dwords
+0x00: pointer to decoded texture start array
+0x04: pointer to TEXTURES.DAT entry table
+0x08: pointer to LRU residency bitmap (600 entries)
+0x0C: pointer to per-texture metadata array
+0x10: 600 (LRU cache capacity)
+0x14: numTexDatEntries
+0x18: 0 (current cached count)
+0x1C: numTexDatEntries (total)
```

**Per-texture processing loop:**
```
for each texture in TEXTURES.DAT:
    1. Rebase entry pointer: entry += textures_dat_base
    2. Read palette type from entry[+3]:
       - 0 or 3: opaque palette (alpha = 0x00)
       - 1: keyed transparency (index 0 = transparent, others alpha = 0xFF)
       - 2: semi-transparent (alpha = 0x80)
    3. Expand 8-bit paletted data to ARGB32:
       - Source: entry[+8 + palette_size*3] (pixel data after RGB palette)
       - For each of 0x1000 pixels (64x64):
         * Look up palette[index*3] -> R, G, B
         * Write {alpha, R, G, B} to 4-byte output
    4. Determine D3D texture format slot:
       - Type 1 -> slot 1 (binary alpha)
       - Type 3 with d3d_exref+0xa5c == 0 -> slot 3
       - Type 2 or 3 -> slot 2 (multi-bit alpha)
       - Other -> slot 0 (no alpha)
    5. Get pixel format masks via DXD3DTexture::GetMask(formatType)
    6. Call ParseAndDecodeCompressedTrackData() to convert ARGB32 to device-native format
    7. Store result in decoded texture buffer
```

**Texture type -> D3D format mapping:**
| Palette Type | Alpha | DXD3DTexture Format Slot |
|-------------|-------|--------------------------|
| 0 (opaque) | 0x00 | 0 (16-bit no alpha) |
| 1 (keyed) | 0x00/0xFF | 1 (16-bit 1-bit alpha) |
| 2 (semi) | 0x80 | 2 (16-bit multi-bit alpha) |
| 3 (opaque alt) | 0x00 | 0 or 3 depending on driver caps |

### ParseAndDecodeCompressedTrackData (0x430d30)

Converts ARGB32 source pixels to device-native pixel format using bitmask packing:

```
1. Extract ARGB channel masks from param struct (offsets +0x0C, +0x10, +0x14, +0x18)
2. For each mask: compute shift position and bit width
3. Determine output pixel size:
   - If total bits <= 16: write uint16 per pixel
   - If total bits > 16: write uint32 per pixel
4. For each MIP level (param[+0x24] downto param[+0x20]):
   - dimension = mip_size_table[level]
   - scale = base_dimension / mip_dimension
   - For each output pixel:
     * Sample source at scaled position
     * Clamp each channel to 0..255
     * Shift and mask into packed format:
       pixel = (A >> (8-Abits)) << Ashift |
               (R >> (8-Rbits)) << Rshift |
               (G >> (8-Gbits)) << Gshift |
               (B >> (8-Bbits)) << Bshift
5. Return total bytes written
```

### UploadRaceTexturePage (0x40b590)

Final upload to D3D texture system:
```
1. If textureIndex == 4 (special env map page):
   - Pre-process: generate alpha from color (alpha = 0x80 if any color, 0x00 if black)
2. Switch on formatMode:
   - 0: DXD3DTexture::LoadRGBS24(data, index, 0)    // 24-bit RGB
   - 1: DXD3DTexture::LoadRGBS32(data, index, 7, 0)  // 32-bit RGBA slot 7
   - 2: DXD3DTexture::LoadRGBS32(data, index, 8, 0)  // 32-bit RGBA slot 8
3. Record transparency mode in DAT_0048dbac[index]:
   - 0 = opaque (formatMode == 0)
   - 1 = has alpha (formatMode != 0)
   - 2 = environment map with driver stencil support
   - 3 = environment map without stencil
```

### ResampleTexturePageToEntryDimensions (0x442d30)

Nearest-neighbor downsampler when source dimensions don't match the static.hed entry:
```
1. Read target width/height from gStaticHedTextureData[entryIndex * 0x10 + 0x08/0x0C]
2. If widths match: no-op return
3. Compute scale = max(srcWidth, tgtWidth) / min(srcWidth, tgtWidth)
4. Determine bytes per pixel: 4 if entry has alpha flag, else 3
5. For each output row:
   - For each output column:
     * Copy bpp bytes from source at (col * scale, row * scale)
   - Advance source row by (scale * srcWidth * bpp)
6. Writes in-place (overwrites source buffer)
```

---

## 8. TGA Decoder

### DecodeArchiveImageToRgb24 (0x442e00)

Full TGA image decoder supporting multiple pixel formats and RLE compression.
Decodes to packed RGB24 (3 bytes per pixel, R-G-B order).

**TGA Header parsing:**
```
offset 0x00: id_length (1 byte)
offset 0x01: colormap_type (1 byte)
offset 0x02: image_type (1 byte)
offset 0x05: colormap_length (2 bytes, LE)
offset 0x0C: width (2 bytes, LE)
offset 0x0E: height (2 bytes, LE)
offset 0x10: bits_per_pixel (1 byte)
offset 0x11: descriptor (1 byte: bit 5 = top-to-bottom, bit 4 = right-to-left)
```

**Supported image types:**

| Type | Description | Implementation |
|------|-------------|---------------|
| 1 | Uncompressed colormapped | Palette lookup: index -> BGR24, swap to RGB |
| 2 | Uncompressed true-color | Handles 16bpp (5-5-5), 24bpp (BGR->RGB), 32bpp (BGRA->RGB, skip alpha) |
| 9 | RLE colormapped | RLE packets: raw runs (< 0x80) copy N+1 palette lookups; repeat runs (>= 0x80) repeat one color N-127 times |
| 10 | RLE true-color (24bpp only) | Same RLE scheme but with inline BGR24 pixels |

**Post-processing:**
- If descriptor bit 5 is clear (bottom-up): flip image vertically (swap rows from top/bottom inward)
- If descriptor bit 4 is set (right-to-left): flip each row horizontally (swap pixels from left/right inward)

---

## 9. Memory Management

### HeapAllocTracked (0x430cf0)

```c
void* HeapAllocTracked(SIZE_T size) {
    void* ptr = HeapAlloc(gGameHeapHandle, 0, size);
    gGameHeapAllocTotal += size;
    if (gGameHeapAllocTotal > gGameHeapAllocPeak)
        gGameHeapAllocPeak = gGameHeapAllocTotal;
    return ptr;
}
```

- Uses Win32 `HeapAlloc` on a private heap (`gGameHeapHandle`)
- Tracks cumulative allocation total and peak watermark
- **No per-allocation size tracking** -- cannot free individual blocks and update total
- This is intentional: the entire heap is destroyed and recreated between races

### ResetGameHeap (0x430cb0)

```c
void ResetGameHeap() {
    if (DAT_004aee4c != 0)
        HeapDestroy(gGameHeapHandle);
    gGameHeapHandle = HeapCreate(0, 24000000, 0);  // 24 MB initial commit
    gGameHeapAllocTotal = 0;
    DAT_004aee4c = 1;
}
```

- **Destroys the entire heap** and creates a fresh 24 MB heap
- Called at the start of `InitializeRaceSession` before any asset loading
- This is a bulk-free strategy: all race assets are freed in one HeapDestroy call
- The 24 MB initial commit covers all track + vehicle + texture data
- `DAT_004aee4c` is a "heap initialized" flag (prevents destroying uninitialized heap)

### Allocation Pattern Summary

```
Race start:
  HeapDestroy(old) + HeapCreate(24MB)     // wipe everything
  HeapAllocTracked(strip_data)            // ~50-200 KB
  HeapAllocTracked(left_trk)              // ~10-50 KB
  HeapAllocTracked(right_trk)             // ~10-50 KB
  HeapAllocTracked(traffic_bus)           // ~5-20 KB
  HeapAllocTracked(levelinf)              // ~100 bytes
  HeapAllocTracked(models_dat)            // ~500 KB - 2 MB
  HeapAllocTracked(textures_dat)          // ~200 KB
  HeapAllocTracked(static_hed)            // ~5-20 KB
  HeapAllocTracked(vehicle_models)        // ~300 KB - 1 MB (all 6 slots, 32-byte aligned)
  HeapAllocTracked(traffic_models)        // ~200 KB (6 traffic types)
  HeapAllocTracked(texture_cache)         // variable (LRU metadata + decoded textures)
  HeapAllocTracked(sky_mesh)              // ~50-100 KB
  ... (particles, HUD, etc.)

Race end:
  HeapDestroy(gGameHeapHandle)            // instant bulk free
```

Temporary buffers (512KB, 256KB for texture compositing) use CRT `malloc/free` instead of the
tracked heap, since they're freed within the same function.

---

## 10. ZIP File Inventory

### ZIP Archives Used by the Game

| ZIP File | Location | Contents |
|----------|----------|----------|
| `level%03d.zip` | Game data dir | Per-track: STRIP.DAT, STRIPB.DAT, LEFT.TRK, LEFTB.TRK, RIGHT.TRK, RIGHTB.TRK, TRAFFIC.BUS, TRAFFICB.BUS, CHECKPT.NUM, LEVELINF.DAT, MODELS.DAT, TEXTURES.DAT, FORWSKY.TGA, BACKSKY.TGA |
| `static.zip` | Game data dir | Shared: static.hed, tpage0..N.dat, SKY.PRR |
| `traffic.zip` | Game data dir | Traffic: model%d.prr, skin%d.tga (per track index) |
| `environs.zip` | Game data dir | Environment maps: named entries per track |
| `LOADING.ZIP` | Game data dir | Loading screen images: load%02d.tga |
| Car ZIPs (via gCarZipPathTable) | Game data dir | Per-car: himodel.dat, carparam.dat, CARSKIN%d.TGA, CARHUB%d.TGA, sound banks |

### Per-Car ZIP Contents

Each car's ZIP (path from `gCarZipPathTable[carTypeIndex]`) contains:
- `himodel.dat` -- High-detail 3D model (PRR format)
- `carparam.dat` -- 268-byte physics/tuning parameters
- `CARSKIN%d.TGA` -- 256x256 car body texture (multiple paint variants)
- `CARHUB%d.TGA` -- 64x64 wheel hub texture
- Sound bank files (loaded by `LoadVehicleSoundBank`)

### Asset Loading Flow Diagram

```
                    InitializeRaceSession (0x42AA10)
                              |
              +---------------+------------------+
              |               |                  |
    LoadTrackRuntimeData   LoadTrackTextureSet  LoadRaceVehicleAssets
    (level%03d.zip)        (static.zip +         (car ZIPs + traffic.zip)
              |             level%03d.zip)              |
     STRIP.DAT             |          |         himodel.dat
     LEFT/RIGHT.TRK    static.hed  TEXTURES.DAT  carparam.dat
     TRAFFIC.BUS           |          |            CARSKIN.TGA
     LEVELINF.DAT    BuildTrackTextureCacheImpl    CARHUB.TGA
     CHECKPT.NUM           |                     model%d.prr (traffic)
                    ParseAndDecode               skin%d.tga (traffic)
                           |
                    D3D Texture Upload
                           |
              +------------+-------------+
              |            |             |
    LoadRaceTexturePages  LoadEnvTex   PreloadLevel
    (static.zip +         (environs.zip)
     level%03d.zip +
     car ZIPs +
     traffic.zip)
              |
     tpage%d.dat  -> UploadRaceTexturePage -> DXD3DTexture::LoadRGBS24/32
     FORWSKY.TGA
     CARSKIN%d.TGA (composited pairs)
     CARHUB%d.TGA (composited grid)
     skin%d.tga
```

---

## Appendix: Function Reference

| Address | Name | Module |
|---------|------|--------|
| 0x10013060 | CreateArchiveProviderManager | M2DX.dll |
| 0x100130d0 | DestroyArchiveProviderManager | M2DX.dll |
| 0x100131a0 | ArchiveProviderManager_OpenPath | M2DX.dll |
| 0x10013250 | ArchiveProviderManager_ClosePath | M2DX.dll |
| 0x100132f0 | ArchiveProviderManager_TryOpenWithProviders | M2DX.dll |
| 0x10013390 | EnsureTrailingBackslash | M2DX.dll |
| 0x10013be0 | ClearArchiveProviderTable | M2DX.dll |
| 0x10013c10 | GetArchiveProviderCount | M2DX.dll |
| 0x10013c50 | CopyArchiveProviderRecords | M2DX.dll |
| 0x10013c90 | CopyArchiveProviderRecordTemplate | M2DX.dll |
| 0x10013cd0 | ArchiveProviderManager_QueryInterface | M2DX.dll |
| 0x10013d00 | ArchiveProviderManager_AddRef | M2DX.dll |
| 0x10013d10 | ReleaseArchiveProviderManager | M2DX.dll |
| 0x42AA10 | InitializeRaceSession | TD5_d3d.exe |
| 0x42fb90 | LoadTrackRuntimeData | TD5_d3d.exe |
| 0x42f990 | LoadEnvironmentTexturePages | TD5_d3d.exe |
| 0x430cb0 | ResetGameHeap | TD5_d3d.exe |
| 0x430cf0 | HeapAllocTracked | TD5_d3d.exe |
| 0x430d30 | ParseAndDecodeCompressedTrackData | TD5_d3d.exe |
| 0x40b1c0 | BuildTrackTextureCache | TD5_d3d.exe |
| 0x40b1d0 | BuildTrackTextureCacheImpl | TD5_d3d.exe |
| 0x40b590 | UploadRaceTexturePage | TD5_d3d.exe |
| 0x43fbc0 | DecompressTrackDataStream | TD5_d3d.exe |
| 0x43fc80 | ParseZipCentralDirectory | TD5_d3d.exe |
| 0x4405b0 | DecompressZipEntry | TD5_d3d.exe |
| 0x440790 | ReadArchiveEntry | TD5_d3d.exe |
| 0x4409b0 | GetArchiveEntrySize | TD5_d3d.exe |
| 0x442560 | LoadStaticTrackTextureHeader | TD5_d3d.exe |
| 0x442670 | LoadTrackTextureSet | TD5_d3d.exe |
| 0x442770 | LoadRaceTexturePages | TD5_d3d.exe |
| 0x442cf0 | FindArchiveEntryByName | TD5_d3d.exe |
| 0x442d30 | ResampleTexturePageToEntryDimensions | TD5_d3d.exe |
| 0x442e00 | DecodeArchiveImageToRgb24 | TD5_d3d.exe |
| 0x443280 | LoadRaceVehicleAssets | TD5_d3d.exe |
| 0x4431c0 | LoadTrafficVehicleSkinTexture | TD5_d3d.exe |
| 0x44867c | fopen_game | TD5_d3d.exe |
| 0x447fe2 | InflateDecompress | TD5_d3d.exe |
