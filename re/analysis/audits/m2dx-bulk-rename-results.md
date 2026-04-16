# M2DX.dll Bulk Dead Code Rename Results

**Date:** 2026-03-28
**Session:** M2DX.dll Ghidra session `81b8ab60f3e949bdb5bdda7f72d08d0d`

## Summary

- **Total M2DX functions:** 422
- **Functions renamed (DEAD_ prefix):** 234
- **Active named functions (unchanged):** 188
- **Remaining unnamed (FUN_):** 0

All previously unnamed `FUN_` functions in M2DX.dll have been renamed with a `DEAD_` prefix.

## Mapping Details

The confirmed mapping `M2DX_addr + 0x400000 = EXE_addr` (or equivalently, `M2DX_Ghidra_addr - 0x10000000 + 0x400000 = EXE_addr`) only produces exact function entry point matches for 10 functions. The two binaries share source code but have different compilation/linking layouts, causing function boundaries to differ.

Multiple matching strategies were used:

| Strategy | Functions Matched |
|---|---|
| Direct address mapping (exact entry point match) | 2 |
| First 8 bytes + exact size match | 55 |
| First 8 bytes + nearby address + size match | 11 |
| 10-instruction mnemonic sequence + size match | 18 |
| 20-instruction mnemonic sequence match | 1 |
| Inside named EXE function (sub-function) | 34 |
| Inside unnamed EXE function (sub-function) | 50 |
| No EXE counterpart found at mapped address | 57 |
| **Total** | **234** (6 matched by other combined heuristics) |

## Rename Categories

### Matched to EXE FUN_ by bytes/mnemonics: 85 functions
These have confirmed byte-level or mnemonic-level matches to specific EXE functions.
Format: `DEAD_FUN_XXXXXXXX` (where XXXXXXXX is the EXE function address)

### Part of named EXE function: 34 functions
The M2DX function address maps to a location inside a named EXE function. These are sub-routines in the DLL that were inlined or merged in the EXE.
Format: `DEAD_<ExeFuncName>_part_XXXX`

Named EXE functions referenced:
- `IntegrateVehiclePoseAndContacts`
- `MainMenuHandler`
- `MainMenu_AnimateIntro`
- `ScreenChampionshipMenu`
- `ChampionshipMenu_HandleInput`
- `MultiplayerLobby_InitUI`
- Various `caseD_` switch handler functions

### Part of unnamed EXE function: 50 functions
Same as above but the containing EXE function is also unnamed.
Format: `DEAD_FUN_XXXXXXXX_part_XXXX`

### CRT library functions: 2 functions
Matched to CRT/runtime library functions (`__ftol`, etc.).

### Other named matches: 6 functions
Matched to specifically named EXE functions (e.g., `MainMenuHandler`, `_strchr`, `caseD_3`, etc.).

### Unmapped: 57 functions
No EXE function exists at or containing the mapped address. These addresses fall in gaps between EXE functions or in non-code regions.
Format: `DEAD_UNMAPPED_XXXXXXXX`

## Notes

1. The M2DX.dll contains 188 actively-used exported API functions (Environment, Create, Destroy, BeginScene, etc.) that were NOT renamed.

2. The 234 DEAD_ functions include a mix of:
   - Dead copies of game logic (e.g., MainMenuHandler, screen handlers)
   - Dead copies of CRT runtime code (_malloc, __ftol, _strchr, etc.)
   - Helper functions that exist as separate functions in the DLL but are inlined in the EXE

3. None of the 141 non-byte-matched functions had zero callers -- they are all referenced internally within M2DX. This suggests they may be DLL-specific implementations rather than true dead copies.

## Completing Remaining Work

The bulk rename is **complete** -- all 234 FUN_ functions have been renamed. No further action needed for the rename pass.

To improve the 57 `DEAD_UNMAPPED_` names in the future:
1. Use Ghidra's Function ID (FID) or Binary Diffing plugins to match by structural similarity
2. Cross-reference with the IDA FLAIR signatures if available
3. Manual analysis of the function bodies to identify their purpose

## Files Generated

- `m2dx_final_state.json` - Complete function list with dead/active classification
- `exe_func_bytes.json` - EXE function first bytes for matching
- `exe_func_mnem20.json` - EXE function mnemonic sequences
- `exe_containing.json` - EXE function containment analysis for unmapped addresses
- `matches.json` - Byte/mnemonic match results
- `rename_log.json` - Detailed rename log
