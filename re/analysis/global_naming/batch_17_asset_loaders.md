---
batch: 17
area: asset_loaders
tier: T4
target_todos: [todo_precise_port_extend_geometry_render_2026-05-17, todo_precise_port_extend_particle_render_2026-05-17]
ghidra_session: 7b1e9504002e420081bb299fbe94a0c2
analyzed_addresses: 0x0043fc80, 0x004405b0, 0x00440790, 0x00440860, 0x004409b0, 0x00442cf0, 0x00442e00, 0x0043fbc0, 0x0043fb70, 0x0043fb90, 0x00447490, 0x00447502, 0x004474f6, 0x00447fe2, 0x00447aa6, 0x00431190, 0x0040ac00, 0x0040b9f0, 0x0040b1d0, 0x00412030, 0x004122f0, 0x0042f990, 0x00442770, 0x0042fad0, 0x00443280
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Asset loaders (ZIP archive, TGA decode, mesh prepare)

## Summary

- Functions analyzed: 25 (5 ZIP I/O + 4 inflate state machine + 4 TGA/static.hed + 6 mesh/texture cache + 6 callers/initializers walked via xref)
- Unnamed DAT_* globals encountered: 22 (after de-dup)
- Already-named globals encountered: 17 (`gStaticHedEntryArray`, `gStaticHedEntryCount`, `gStaticHedTextureData`, `gTrackTextureCount`, `gCarZipPathTable`, `gSlotCarTypeIndex`, `gSlotMeshResourcePtrTable`, `g_playerReflectionMeshResource`, `gModelsDatEntryCount`, `gModelsDatEntryTable`, `gVehiclePhysicsTable`, `gVehicleTuningTable`, `gReverseTrackDirection`, `gTrafficActorsEnabled`, `g_trackPoolIndex`, `g_vehicleProjectionEffectMode`, `g_selectedGameType`)
- Proposals — high confidence: 19
- Proposals — medium confidence: 4
- Proposals — comment-only (low confidence): 0

## Methodology

Entry points walked from `td5_asset.c` comment block at the top:
- ZIP archive: `ParseZipCentralDirectory @ 0x0043fc80`, `DecompressZipEntry @ 0x004405b0`, `ReadArchiveEntry @ 0x00440790`, `OpenArchiveFileForRead @ 0x00440860`, `GetArchiveEntrySize @ 0x004409b0`, plus helpers `DecompressTrackDataStream @ 0x0043fbc0`, `ReadTrackStaticDataChunk @ 0x0043fb70`, `ReadCompressedTrackStreamChunk @ 0x0043fb90`.
- DEFLATE: `InflateDecompress @ 0x00447fe2`, `InflateFlushOutputAndUpdateCrc32 @ 0x00447490`, `InflateProcessStoredBlock @ 0x00447aa6`, `InflateWriteOutputChunk @ 0x00447502`, `InflateRefillInputBuffer @ 0x004474f6`.
- TGA + asset table: `DecodeArchiveImageToRgb24 @ 0x00442e00`, `FindArchiveEntryByName @ 0x00442cf0`, `LoadFrontendTgaSurfaceFromArchive @ 0x00412030`, `LoadTgaToFrontendSurfaceFromArchive @ 0x004122f0`.
- Mesh prepare: `PrepareMeshResource @ 0x0040ac00`, `ParseModelsDat @ 0x00431190`, `GetTextureSlotStatus @ 0x0040b9f0`, `BuildTrackTextureCacheImpl @ 0x0040b1d0`.
- Texture upload pipelines: `LoadRaceTexturePages @ 0x00442770`, `LoadEnvironmentTexturePages @ 0x0042f990`, `LoadRaceVehicleAssets @ 0x00443280`, `InitializeTrackStripMetadata @ 0x0042fad0`.

Relevance gate: a global was in-scope if it (a) backs the ZIP I/O streaming state machine, (b) backs the inflate decompressor bit/byte cursor or CRC accumulator, (c) is read/written exclusively by the asset-load and texture-cache builders, or (d) is the cardef/mesh-cache slot table that maps actors to loaded model buffers. ZIP-handle and inflate-cursor globals dominate this batch because the original spreads decompression state across 12 file-scope globals with no struct around them.

## Proposals

### ZIP-archive streaming state (declared in port comment block at td5_asset.c:16-26)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3760 | void* | `g_zipIoBuffer64K` | **high** | 64KB scratch buffer allocated in `ParseZipCentralDirectory @ 0x0043fc80` (`_malloc(0x10000)`), freed in `DecompressZipEntry`. 41 xrefs across ZIP-streaming functions. Read at every `*(byte *)(DAT_0047b1d4 + (int)DAT_004c3760)` byte-fetch in `DecompressTrackDataStream`. Matches port's `0x4C3760  64KB I/O buffer` annotation. | td5_asset.c:21 comment `0x4C3760  64KB I/O buffer` |
| 0x004cf974 | FILE* | `g_zipDecompressOutputFile` | high | Read in `ReadCompressedTrackStreamChunk @ 0x0043fb90` as `_fwrite(DAT_004c3764, 1, DAT_0047b1f4, DAT_004cf974)`. Only used in the file-streaming branch of `DecompressZipEntry` when output target is a FILE handle (vs an in-memory buffer). 2 xrefs. | (none — port writes directly to in-memory buffer; the FILE* output path is unused in port) |
| 0x004cf97c | FILE* | `g_zipArchiveFileHandle` | **high** | Current ZIP archive FILE handle. Written by `ParseZipCentralDirectory` and `OpenArchiveFileForRead` (via `fopen_game`); read by all `fread_game`/`fseek_game`/`fclose_game` in the streaming machine. 41 xrefs. Matches port's `0x4CF97C  current ZIP file handle` annotation. | td5_asset.c:22 comment `0x4CF97C  current ZIP file handle` |
| 0x004cf978 | uint | `g_zipCdRemainingBytes` | **high** | Central-directory remaining-bytes counter. Written by `ParseZipCentralDirectory` as `uVar7 = DAT_004cf978 - DAT_004cf980` (subtract chunk size each refill); read at chunk-boundary refill in `DecompressTrackDataStream`. 25 xrefs all in `ParseZipCentralDirectory` and `DecompressTrackDataStream`. | (none — port uses miniz central-directory walker) |
| 0x004cf980 | uint | `g_zipCdChunkSize` | **high** | Per-iteration chunk size for streaming the central directory (`= min(remaining, 0x10000)`). 44 xrefs in the same two functions. Always set immediately before `fseek + fread` in the streaming refill block. | (none — same reason) |
| 0x004cf984 | uint | `g_zipCdFileOffset` | **high** | Current file offset (in archive) of the next CD chunk to read. 30 xrefs. Written by `ParseZipCentralDirectory` initial seek (`= 0x10000` cap, then `=` ftell — header offset), then advanced by `+= DAT_004cf980` after each chunk. Matches port's `0x4CF984  central directory file offset` annotation. | td5_asset.c:23 comment `0x4CF984  central directory file offset` |
| 0x004cf988 | uint | `g_zipLocalHeaderFileOffset` | **high** | File offset of the matched local header (result of CD search). Written by `ParseZipCentralDirectory` as the relative-offset field of the matched entry; read by `ReadArchiveEntry`/`OpenArchiveFileForRead` to `fseek_game(handle, DAT_004cf988, 0)` before calling `DecompressZipEntry`. 3 xrefs. Matches port's `0x4CF988  local header offset` annotation. | td5_asset.c:24 comment `0x4CF988  local header offset` |
| 0x004c3764 | void* | `g_zipDecompressOutputPtr` | **high** | Destination buffer for decompressed bytes. Written by `OpenArchiveFileForRead` and `ReadArchiveEntry` (= `param_3` or freshly malloc'd buffer) before calling `DecompressZipEntry`; read by `_fwrite`, `fread_game`, and the CRC32 loop inside `DecompressZipEntry`. 8 xrefs. Matches port's `0x4C3764  output destination pointer for decompression` annotation. | td5_asset.c:25 comment `0x4C3764  output destination pointer for decompression` |
| 0x0047b1d4 | uint | `g_zipCdReadCursor` | **high** | Cursor within the 64K I/O buffer (`g_zipIoBuffer64K`). Reset to `0x10000` to trigger refill, then advanced byte-by-byte by `DecompressTrackDataStream`/`InflateRefillInputBuffer`. 56 xrefs (mostly in `ParseZipCentralDirectory`'s repeated inline 4-byte/2-byte read macro). Matches port's `0x47B1D4  read cursor within 64KB buffer` annotation. | td5_asset.c:23 comment `0x47B1D4  read cursor within 64KB buffer` |
| 0x0047b1dc | uint | `g_zipExpectedCrc32` | **high** | Expected CRC32 from the ZIP local header. Written by `DecompressZipEntry @ 0x004405b0` after reading the local header (`*(uint *)((int)DAT_004c3760 + 0xe)`); compared against `g_zipRunningCrc32` at end of decompression. 3 xrefs. Matches port's `0x47B1DC  expected CRC32 from ZIP header` annotation. | td5_asset.c:24 comment `0x47B1DC  expected CRC32 from ZIP header` |
| 0x0047b1d8 | uint | `g_zipRunningCrc32` | **high** | Running CRC32 accumulator over decompressed bytes. Updated by both `DecompressZipEntry` (stored-block branch) and `InflateFlushOutputAndUpdateCrc32 @ 0x00447490` via `&DAT_00475160` (CRC32 table). 14 xrefs. Matches port's `0x47B1D8  running CRC32 of decompressed data` annotation. | td5_asset.c:25 comment `0x47B1D8  running CRC32 of decompressed data` |
| 0x0047b1ec | u32 (bool) | `g_zipDecompressTargetIsFile` | high | Flag selecting "decompress to in-memory buffer" (=0, `ReadArchiveEntry`/`OpenArchiveFileForRead`) vs "decompress to FILE handle" (=1, branch in `DecompressZipEntry` writing via `_fwrite`). 5 xrefs. Cleared at the entry of both reader-into-buffer paths. | (none — port always targets in-memory buffer) |

### DEFLATE / inflate decompressor state machine

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0047b1c8 | uint | `g_inflateBitBuffer` | **high** | The 32-bit bit-buffer accumulator used by `InflateDecompress @ 0x00447fe2`, `InflateProcessStoredBlock @ 0x00447aa6`, `InflateDecodeHuffmanCodes`, `InflateProcessFixedHuffmanBlock`, `InflateProcessDynamicHuffmanBlock`. 9 xrefs. Refilled from `g_zipIoBuffer64K` 8 bits at a time. | td5_inflate.c bit accumulator (TINFL bit_buf) |
| 0x0047b1cc | uint | `g_inflateBitCount` | **high** | Number of valid bits in `g_inflateBitBuffer`. 69 xrefs — most-touched inflate global; incremented by 8 on byte refill, decremented per code/value consumed. | td5_inflate.c num_bits |
| 0x0047b1d0 | uint | `g_inflateOutputPosition` | **high** | Byte position within the inflate output buffer (relative to `g_inflateOutputBase`). 10 xrefs. Used to track how many bytes have been emitted in the current flush window; reaches 0x8000 → triggers `InflateFlushOutputAndUpdateCrc32`. | td5_inflate.c out_buf position |
| 0x0047b1e0 | byte* | `g_inflateOutputBase` | **high** | Base pointer to the current output region. Seeded from `g_zipDecompressOutputPtr` at the start of `InflateDecompress` (`DAT_0047b1e0 = DAT_004c3764`); advanced by chunk size in the file-output branch of `InflateFlushOutputAndUpdateCrc32`. 7 xrefs. | td5_inflate.c output_base |
| 0x0047b1e4 | byte* | `g_inflateInputBase` | **high** | Base pointer to the current input region (= `g_zipIoBuffer64K` at start of `InflateDecompress`). Read at every `(DAT_0047b1d4 + DAT_0047b1e4)` byte-fetch in the inflate state machines. 16 xrefs. | td5_inflate.c input_base |
| 0x0047b1e8 | uint | `g_inflateTotalBytesOut` | high | Total bytes decompressed so far (advanced by chunk size in `InflateFlushOutputAndUpdateCrc32`). Read at end of `InflateDecompress` as the return value when CRC matches. 3 xrefs. | td5_inflate.c total_out |
| 0x0047b1f0 | size_t | `g_inflateChunkBytesWritten` | high | Result of the chunk-write fwrite in `ReadCompressedTrackStreamChunk @ 0x0043fb90` (`DAT_0047b1f0 = _fwrite(...)`). Compared against requested chunk size by `InflateFlushOutputAndUpdateCrc32` to decide whether to update CRC32. 2 xrefs. | (none — port has unified in-memory output) |
| 0x0047b1f4 | uint | `g_inflateChunkBytesRequested` | high | Number of bytes the inflate state machine wants the host to write (set by `InflateWriteOutputChunk @ 0x00447502` from EAX, read by `ReadCompressedTrackStreamChunk`). 2 xrefs. Companion to `g_inflateChunkBytesWritten`. | (none — same reason) |

### Texture-page upload & cache state

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3d04 | u32 (bool) | `g_textureUploadSkipResample` | **high** | "Skip nearest-neighbour resample on upload" flag. Read at 7 sites (all `ResampleTexturePageToEntryDimensions` gates in `LoadRaceTexturePages` / `LoadEnvironmentTexturePages` / `BuildTrackTextureCacheImpl`'s upload loop, and inside `LoadTrafficVehicleSkinTexture @ 0x004431c0`). When non-zero, the 256×256 source page is uploaded as-is; when zero (default), it's resampled to the static.hed entry's stored dimensions. Tied to the texture-config mode set somewhere in track loading. | td5_render.c upload-time resample gate |
| 0x004aee10 | byte* | `g_perTrackEnvAssetTable` | **high** | Per-track environment-asset descriptor table base. Written by `InitializeTrackStripMetadata @ 0x0042fad0` from `&DAT_0046bb1c + track_idx*4` (table of pointers). Header at +0 = count (DAT_004aee10[0]); entries stride 0x20 starting at +0x40 hold archive name strings used by `LoadEnvironmentTexturePages` to fetch each `ENV%d` page. 11 xrefs. | td5_asset.c env-list parser |
| 0x0046bb1c | void*[N] | `g_perTrackEnvAssetTableArray` | high | Constant table of per-track env-asset-table pointers (indexed by `g_trackPoolIndex`). Only reader: `InitializeTrackStripMetadata @ 0x0042fad4`. 1 xref. | td5_asset.c per-track env table |
| 0x00466ec8 | u8[6] | `g_slotCarTypeIndexAlt` | high | Second per-slot car-type index table at +4 from `gSlotCarTypeIndex @ 0x00466ec4`. Read at `LoadRaceTexturePages @ 0x004429d7` as `(&gCarZipPathTable)[(&DAT_00466ec8)[iVar9]]` to fetch the **alt-color skin** archive path for the same slot (the loop interleaves primary skin into the first half of a 64×128 image and alt skin into the second half). Writers at 0x0042aeab / 0x0042aeca (within `InitializeRaceSession`). 5 xrefs. | (none — port currently only loads primary skin) |

### Per-vehicle cache / cardef table aliases (filled by `LoadRaceVehicleAssets`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3d48 | u32[6+1] | `g_slotMeshResourceSize` | **high** | Per-slot allocation size for the himodel.dat blob (one dword per slot, 6 slots + 1 trailing entry for player reflection size). Written at `LoadRaceVehicleAssets @ 0x004432d7` (= `GetArchiveEntrySize(s_himodel_dat, ...) rounded up`); read at 0x4432ec / 0x4433e2 / 0x44351b. 4 xrefs. | td5_asset.c himodel blob size table |
| 0x004c3d78 | byte* | `g_chassisStaticHedEntryPtr` | high | Pointer to the "chassis" static.hed entry, fetched once in `LoadRaceVehicleAssets @ 0x00443337` (`= FindArchiveEntryByName(..., s_chassis_00474e14)`). Read at 0x00443741 to seed the per-track lighting UV patch. 2 xrefs. | (none — chassis-entry lookup is inline in port) |
| 0x004c3d7c..0x004c3d88 | f32[4] | `g_chassisFixedPointBounds` | medium | 4 float globals seeded immediately after `g_chassisStaticHedEntryPtr` from `(uint *)(DAT_004c3d78 + 0x2c..0x38) * _g_fixedPointToFloatScale`. These are the chassis bounding-box extents in float space (used by mesh-prepare / sky-clip downstream). Comment-only group naming; individual semantic of each component (min_x/min_y/max_x/max_y?) needs follow-up. | (none) |
| 0x004c3d28 | u32[6] | `g_trafficMeshSlotOffset` | high | Per-traffic-slot offset into the traffic-mesh heap allocation. Written at `LoadRaceVehicleAssets @ 0x0044347b` (iVar19 accumulator) and patched-relative to the heap base in the 0x4c3d28-loop just below. Reads at 0x004c3d40 (loop bound) — gives 6 slots × 4 bytes. | td5_asset.c traffic-mesh per-slot offset table |
| 0x004c3d60 | u32[6] | `g_trafficMeshSlotSize` | high | Sister table to `g_trafficMeshSlotOffset` — per-slot allocation size for the `MODEL%d.PRR` traffic mesh blob. Written at `LoadRaceVehicleAssets @ 0x0044349a`; read at 0x004433f0. | td5_asset.c traffic-mesh per-slot size table |
| 0x004ae8c8 | struct[6] | `g_trafficActorTuningTable` | high | Per-traffic-slot tuning record. Each entry is 0x23 dwords (mirrors `gVehicleTuningTable` layout). Written in the traffic-init loop of `LoadRaceVehicleAssets` by **copying** `gVehicleTuningTable` (the player template) into each of 6 slots. Loop bound: `< 0x4aec10` ⇒ 6 entries × 0x8C bytes = 0x348 bytes ending at 0x004aec10. 2 xrefs. | td5_ai.c traffic vehicle tuning |
| 0x004aee54 | uint* | `g_modelsDatPostEntryTableEnd` | medium | Set in `ParseModelsDat @ 0x0043119e` as `gModelsDatEntryTable + gModelsDatEntryCount * 2` — i.e. the address just past the end of the 8-byte-pair entry table. Allows downstream code (currently unidentified callers) to find the start of the raw mesh blob region. 1 xref (writer only — no reader yet observed, which matches the audit memo's note that the 2nd dword of each pair has no observed runtime consumer). | td5_asset.c MODELS.DAT end-of-table |

## Key discoveries

1. **The ZIP/inflate decompression machinery is implemented across 18 file-scope globals with NO encapsulating struct.** The original arrangement is essentially "a single global zlib stream object exploded into individual labels":
   - I/O cursor pair (`g_zipCdReadCursor` / `g_zipIoBuffer64K`) — bytes 0..64K
   - CD streaming triple (`g_zipCdFileOffset` / `g_zipCdRemainingBytes` / `g_zipCdChunkSize`) — for the central-directory scan
   - Bit-stream pair (`g_inflateBitBuffer` / `g_inflateBitCount`) — for the DEFLATE huffman decoder
   - Output triple (`g_inflateOutputBase` / `g_inflateOutputPosition` / `g_inflateTotalBytesOut`)
   - CRC32 pair (`g_zipExpectedCrc32` / `g_zipRunningCrc32`)
   - Target mode flag (`g_zipDecompressTargetIsFile`) and the chunk-bytes pair (`g_inflateChunkBytesRequested` / `g_inflateChunkBytesWritten`) that connect inflate to the host fwrite hook
   The port collapses all of these into stack/heap state inside `td5_inflate.c` and `td5_asset.c`, which is the right architectural call but makes future Frida-hook diffing harder if a regression shows up in the loader. **Recommend exposing these globals in the port struct `TD5_ZipStreamState` if precise-port of the loader is ever attempted.**

2. **`g_zipDecompressTargetIsFile` (0x0047b1ec) selects between two DIFFERENT inflate paths.** In `DecompressZipEntry` with `target_is_file == 0`, the code reads the entire compressed entry in one go, runs inflate to memory, and CRCs in a single pass. With `target_is_file == 1`, it does a chunked fread + chunked CRC + chunked fwrite cycle. The `fwrite` target is `g_zipDecompressOutputFile`. **In TD5_d3d.exe, no observed call site sets `target_is_file = 1`** — `ReadArchiveEntry` and `OpenArchiveFileForRead` both explicitly write `0` at function entry. The file-output branch appears to be dead code (legacy support for extracting a ZIP entry directly to disk). The port omits it entirely.

3. **`FindArchiveEntryByName @ 0x00442cf0` has a "default to entry 0 on no-match" failure mode that is easy to miss.** The decompiled C is:
   ```c
   pbVar2 = gStaticHedEntryArray;
   if (uVar3 != gStaticHedEntryCount) pbVar2 = pbVar4;
   return pbVar2;
   ```
   On a stricmp-miss, the function returns `gStaticHedEntryArray[0]` (the FIRST entry) instead of NULL. This means a typo or missing entry in `static.hed` would silently pick up the wrong texture rather than crashing. Worth a note for port robustness — if any port site relies on a NULL return from a name-lookup helper, that's a divergence from the original. (Quick scan of `td5_asset.c` suggests the port uses explicit `for`-loops with `return NULL`, not byte-faithful to original behavior. Worth a comment in the port.)

4. **The TGA decoder switch in `DecodeArchiveImageToRgb24 @ 0x00442e00` handles types 1, 2, 9, 10 with 8/16/24/32 bpp inputs and emits packed RGB24** (with channel swap from BGR-on-disk to RGB-out). The flip handling (rows or columns) is gated by bits 5 and 4 of TGA descriptor byte `param_1[0x11]` — the standard TGA Image Descriptor byte. **The port comment in td5_asset.c:11 states this matches `DecodeArchiveImageToRgb24` exactly** — confirmed by static audit.

5. **`g_textureUploadSkipResample` (0x004c3d04) is the SINGLE source of truth for "is the on-disk page size already the upload size?".** It's read at every UploadRaceTexturePage call-site (7 of them) as a gate around `ResampleTexturePageToEntryDimensions`. The flag is written somewhere outside this batch's scope (likely in the texture-config init code; not seen in the asset-load functions analyzed). If the port misses this gate, every page would get a needless resample → measurable load-time hit. **Worth verifying in the port.**

6. **The TGA-decode "image_exref" pseudo-symbol seen in Ghidra (`Image_exref`) is actually `[0x0045d518]`, the import-table pointer to the M2DX.dll-allocated TGA-decode scratch struct.** Despite Ghidra labelling it as a global, it's not internal state — the actual memory lives in `M2DX.dll`'s data segment at `0x000600ac` (the value at 0x0045d518). The port substitutes its own stb_image-based decoder, so this address is correctly **NOT** an asset-loader global to rename — it's an external DLL surface. Listed here only to disambiguate it from the in-scope renames.

7. **`g_slotCarTypeIndexAlt` (0x00466ec8) is at offset +4 from `gSlotCarTypeIndex` and stores a STAGGERED version of the same array.** Memory dump: `[2, 1, 3, 2, 0, ...]` vs `[0, 2, 1, 3, 2, 0]` for the primary table at 0x00466ec4. Reading `(&DAT_00466ec8)[0]` returns `gSlotCarTypeIndex[1]`, etc. This is used in `LoadRaceTexturePages` to load the **co-driver / second-color skin** for the 2nd slot in each grid pair. The original layout suggests slot-pairs of cars (slot 0+1, slot 2+3, slot 4+5) share an atlas page with two skins emitted into top/bottom halves. **The port's td5_asset.c does not currently emit the alt-skin** — this may be one source of the "AI cars all look the same color" visual class if anyone reports it.

8. **`ParseModelsDat @ 0x00431190` walks ONLY the first dword of each 8-byte top-level entry pair.** The second dword is unused by any observed reader (the function's own header comment acknowledges this: *"Current evidence still does not show any runtime consumer for the second dword in each top-level pair"*). `g_modelsDatPostEntryTableEnd` (0x004aee54) is set but never read. **This is a latent format/parser asymmetry** — either the second dword is a never-implemented LOD pointer, or the parser is over-conservative. Useful flag for the geometry-render audit memo (`todo_precise_port_extend_geometry_render_2026-05-17`).

9. **The `g_chassisStaticHedEntryPtr` (0x004c3d78) lookup is performed exactly once per race-start** in `LoadRaceVehicleAssets`, then reused for every per-slot mesh's lighting patch. The 4 floats at +0x2c..+0x38 in the chassis entry (cached at 0x004c3d7c..0x004c3d88 as `g_chassisFixedPointBounds`) are the chassis bounding-box extents pre-converted to float-space via `_g_fixedPointToFloatScale`. Used downstream by `PatchModelUVCoordsForTrackLighting @ 0x00443730`. **The port may inline this lookup per-mesh, which would be slightly slower but functionally equivalent.**

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00475160 | CRC32 lookup table (256 entries × 4 bytes). Static-data constant. Used by both `DecompressZipEntry` and `InflateFlushOutputAndUpdateCrc32`. Already covered by port `s_crc32_table[256]`. | T3 static-data constants batch |
| 0x0045d518 | `PTR_Image_0045d518` — M2DX.dll import-resolved pointer to the DDraw-side TGA decode scratch struct (M2DX heap addr `0x000600ac`). External, do not rename. | (external — M2DX) |
| 0x0048dc3c | Texture cache header pointer (allocated by `BuildTrackTextureCacheImpl`). Read by `GetTextureSlotStatus @ 0x0040b9f0`. | T2.7 sprite-atlas/texture-cache batch (likely covered there) |
| 0x0048dc40 | Texture cache pointer pair (sub-header at +0x0, alt header at +0x4). Same family. | T2.7 same batch |
| 0x0048dc44 | Source MODELS.DAT/static.hed blob ptr stored by `BuildTrackTextureCacheImpl`. | T2.7 same batch |
| 0x0048dc2c | Decompression destination ptr stored by `BuildTrackTextureCacheImpl`. | T2.7 same batch |
| 0x0045d624, 0x0045d61c, 0x0045d620, 0x0045d5f4, 0x0045d618 | Float constants used by `PrepareMeshResource` (UV clamp 0.0/1.0, UV inset bias 1/128 = 0.0078125, UV scale 0.984375 = 1 - 1/64, light min-threshold 0.0366). Already audited in batch_12/render-const batches. | T3 math-const batch |
| 0x00474e1c, 0x00474e14, 0x00474dfc, 0x00474df0, 0x00474dd8, 0x00474cd8, 0x00474ccc, 0x00474cc0, 0x00474cbc, 0x00474cb4, 0x00474ca4, 0x00474c9c, 0x00474c8c, 0x00474c84, 0x00467268, 0x00467294, 0x00473b18, 0x00473b28, 0x00473b30, 0x00463fc0 | Archive entry-name string literals (`himodel.dat`, `chassis`, `carparam.dat`, `MODEL%d.PRR`, `traffic.zip`, `static.zip`, `level%03d.zip`, `environs.zip`, `ENV%d`, `tpage%d.dat`, `BACKSKY.TGA`, `FORWSKY.TGA`, `SKY`, `CAR%d`, `CARSKIN%d.TGA`, `WHEELS`, `CARHUB%d.TGA`, `TRAF%d`, fopen mode `"rb"`). All read-only. | T3 string-literal audit (not naming target — these get .rdata symbols only) |
| 0x00473b30 | 3-byte string `"SKY"` — the prefix the env-list parser uses to flag a page as "sky" vs "ground" via the +0x14 flag write (= 3 for SKY, = 1 otherwise). Tied to sky-dome render path. | T3 sky-dome render batch |
| 0x004aedb0..0x004aedd0 area | CHECKPT.NUM buffer & misc track-data per-track config (already covered by batch_10) | (already covered) |
| 0x004c3d10 | `gSlotMeshResourcePtrTable` — already named (6 dwords). | (already named) |
| 0x004c3d40 | `g_playerReflectionMeshResource` — already named. | (already named) |
| 0x004c3d68 / 0x004c3d6c / etc | Tail entries of the traffic-mesh tables, padding/sentinel. | (out-of-scope detail) |
| 0x00466edc..0x00466ee4 | `gCarZipPathTable` already named (table of pointers to "cars\\xxxxx.zip" strings, 4 entries). | (already named) |
| 0x00466ec0 | 1 entry before `gSlotCarTypeIndex @ 0x00466ec4` — unanalyzed neighbour, possibly the AI-driver-color index baseline. | T3 catch-all |
| 0x004962a4 | Already-named `g_loaderError` (set to 1 by every "cannot open/alloc" error path in the TGA loaders). Worth checking if it's named — appears as `_DAT_004962a4` in decompile. | T3 — verify in symbol table; if unnamed propose `g_assetLoadErrorFlag` |
| 0x00474000+ | Hot zone of asset-related strings and small per-track tables; deserves its own string/strtab batch. | T3 string/strtab batch |
| 0x00495230, 0x00495250, 0x0049525c, 0x004951d8 | TGA-decode auxiliary fields written by some TGA-config init (TGA palette/source data pointers, format/bpp config). Cluster used by the four `LoadTga*FromArchive` variants. | T3 TGA-config batch — possibly worth its own mini-batch given 25+ xrefs into the cluster from 0049525c alone |

## TODO impact

**todo_precise_port_extend_geometry_render_2026-05-17:** Investigation surfaces several globals that the precise-port effort will need to mirror. Most relevant:
- `g_modelsDatPostEntryTableEnd` (0x004aee54) — set but never read in observed code paths. The audit memo notes "MODELS.DAT 2nd dword unused" as an open question; this confirms it from the writer side.
- `g_textureUploadSkipResample` (0x004c3d04) — the upload-time gate that determines whether `ResampleTexturePageToEntryDimensions` runs. The port should verify it honours the same gate, or all texture pages will be force-resampled.
- `g_chassisStaticHedEntryPtr` (0x004c3d78) + `g_chassisFixedPointBounds` (0x004c3d7c..0x004c3d88) — the chassis bounds cache that `PatchModelUVCoordsForTrackLighting` uses. Naming these makes the lighting-UV-patch precise-port substantially clearer.

No closing mechanism in this batch — these are read-only RE notes that LOWER the cost of a future geometry-render precise-port session.

**todo_precise_port_extend_particle_render_2026-05-17:** Tangential. The particle precise-port would need the static.hed entries for SMOKE/RAINSPL/etc. atlases, which are looked up via `FindArchiveEntryByName @ 0x00442cf0` — this batch confirms that function defaults to entry-0 on miss rather than NULL, which is a footgun the port should mirror or guard against. Otherwise nothing in this batch directly bears on the particle audit.

## Ghidra session notes

- Session 7b1e9504002e420081bb299fbe94a0c2 opened TD5_pool0 read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; released via `bash scripts/ghidra_pool.sh cleanup` after analysis.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
