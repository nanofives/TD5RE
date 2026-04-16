# Function Mapping Confidence Audit — 2026-04-08

## Scope

Audit of `TD5_d3d.exe` function naming and documentation coverage using the live
`TD5` Ghidra project as ground truth.

Live function inventory was originally exported from the unlocked `TD5` project to:

- `temp_analysis/td5_inventory_live_t5.tsv`

Post-rename verification export:

- `temp_analysis/td5_inventory_live_t5_renamed.tsv`

Comparison baselines:

- `TD5_headless`, `TD5_mcp`, `TD5_research`, `TD5_temp`
- `re/analysis/exe_func_bytes.json`
- `re/analysis/exe_func_mnemonics.json`
- `re/analysis/exe_func_mnem20.json`
- `re/analysis/archive/ENGINE_DOSSIER.md`

## Live Ghidra State

`TD5` post-rename live export counts:

- Total functions: `876`
- Named functions: `876`
- Auto-named `FUN_...`: `0`
- `USER_DEFINED`: `843`
- `ANALYSIS`: `28`
- `IMPORTED`: `1`
- `DEFAULT`: `4`

The live `TD5` export exactly matches the previously cloned probe snapshot.

## Sync Findings

### In sync

- The live `TD5` project and the cloned `TD5_probe` snapshot are identical.
- `re/analysis/exe_func_bytes.json`, `exe_func_mnemonics.json`, and `exe_func_mnem20.json` have been refreshed from the live post-rename project state:
  - `876` entries each
  - renamed addresses now match live Ghidra, including `RenderPositionerGlyphStrip` and `ReadTaggedParam`

### Out of sync

- `TD5_headless`, `TD5_mcp`, `TD5_research`, and `TD5_temp` are all on an old baseline:
  - `820` total functions
  - `28` named
  - `791` `FUN_...`
- `re/analysis/archive/ENGINE_DOSSIER.md` is stale:
  - says `856` EXE functions
  - says `~504 / 856` live-named in `TD5_d3d.exe`

## Confidence Model

### High-confidence mapped

Definition:

- semantic name present in live Ghidra, and
- backed by analysis docs/session notes, or
- promoted in engine dossier / dedicated analysis notes

Count:

- `624`

Examples:

- `0x00401590 UpdateChaseCamera`
- `0x00404030 UpdatePlayerVehicleDynamics`
- `0x00409e80 CheckRaceCompletionState`
- `0x0040a8c0 BuildRaceResultsTable`
- `0x00414b50 FrontEnd_MainLoop`
- `0x00418c60 NetMsg_EnqueueMessage`
- `0x00421da0 UnlockTrackOrCar`

### Runtime/library named

Definition:

- named, but CRT/import/runtime/helper rather than core game logic

Count:

- `235`

Examples:

- `entry`
- `_rand`
- `__ftol`
- `_malloc`
- `RtlUnwind`
- `DirectSoundCreate`

### Switch fragments

Definition:

- `caseD_...` names representing compiler-generated switch fragments, not stable standalone functions

Count:

- `3`

Entries:

- `0x004173b1 caseD_7`
- `0x00417700 caseD_a`
- `0x0041808d caseD_6`

These match the policy in `gap-functions-naming-wave2.md`: they should be treated
as fragments of parent state machines, not independent mapping failures.

### Unmapped

Definition:

- still `FUN_...` in the live `TD5` project

Count:

- `0`

Status:

- `0x00414f40` has been renamed in the live project to `RenderPositionerGlyphStrip`
- the live `TD5` project now has no remaining `FUN_...` auto-named functions

### Low-confidence game-specific names

Definition:

- semantic name exists in live Ghidra
- not a runtime/CRT/import helper
- no corresponding analysis/session markdown located during repo scan

Count:

- `14`

Entries:

- `0x004539a0 PackYCbCrTableEntry`
- `0x00456f40 LookupQuantTable`
- `0x00458bc9 DecodeJPEGMCU`
- `0x004599d0 YCbCrToRGB_Row_16bit_444`
- `0x00459ea0 YCbCrToRGB_Row_16bit_420`
- `0x0045a3a0 YCbCrToRGB_Row_8bit`
- `0x0045aa75 IDCT_1D_8pt_Float_C`
- `0x0045b1c0 BitstreamRefillBits`
- `0x0045b750 BuildBitplaneDeinterleaveTable`
- `0x0045b7d0 DecodeVQ_Block`
- `0x0045bd00 SetVGAPalette`
- `0x0045bd3c DecompressLZData`
- `0x0045be5f UnpackBitplaneToPixels`
- `0x0045bf08 DecodeBitmapRLERun`

## Practical Reading Of The Results

- The live `TD5` project is heavily mapped already.
- The main unresolved work is not broad naming coverage anymore.
- The real remaining work is:
  - sync stale project copies and stale JSON snapshots to the live `TD5` state
  - validate or document the remaining `14` low-confidence FMV helper names

## Recommended Next Pass

1. Refresh the `exe_func_*.json` snapshots from the live `TD5` project.
2. Investigate the remaining `14` low-confidence FMV helper names as the next evidence pass.
3. Ignore `caseD_...` fragments unless the parent screen handlers need restructuring.
