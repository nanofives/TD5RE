# Unnamed Globals -- Resolved

> Investigation and naming of ~20 previously unnamed global variables flagged in
> `global-variable-catalog.md` Section 7.
>
> Date: 2026-03-28 | Session: 50589c8be1e74a2ca357bf035d019805 (UPDATE mode)

---

## Summary

**Total globals investigated:** 21
**Successfully renamed in Ghidra:** 19
**Types determined:** 15 (retyped where possible)
**Bonus discoveries:** 2 additional globals named during investigation (g_currentViewIndex, g_msgId)

---

## Resolved Globals

### CRT / Application Entry

| Address    | Old Name       | New Name                    | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|-----------------------------|-----------|---------|-------|----------|
| 0x4D0D28   | DAT_004d0d28   | `g_cmdLinePtr`              | char*     | Command line string from GetCommandLineA, stored in CRT entry point | 3 (1W, 2R) | Yes |
| 0x4AEE30   | DAT_004aee30   | `g_appExitCode`             | dword     | Application exit code; returned from GameWinMain as wParam from WM_QUIT | 1 (1R) | Yes |
| 0x4AEE2C   | DAT_004aee2c   | `g_msgId`                   | int       | MSG.message field from PeekMessageA; compared against 0x12 (WM_QUIT) in main loop | 2 (part of MSG struct at 0x4AEE28) | Yes |

### Main Game Loop / State Machine

| Address    | Old Name       | New Name                    | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|-----------------------------|-----------|---------|-------|----------|
| 0x473B6C   | DAT_00473b6c   | `g_dxConfirmGate`           | dword     | DX6 confirmation gate. Non-zero = display mode recovery path; ConfirmDX6() called then gate cleared | 2+ (main loop) | Yes |
| 0x474C00   | DAT_00474c00   | `g_moviePlaybackPending`    | dword     | Intro movie playback flag. Set=1 initially; cleared after FMV plays in GAMESTATE_INTRO | 2 (1R, 1W) | Yes |
| 0x474C04   | DAT_00474c04   | `g_frontendInitPending`     | dword     | Frontend initialization pending flag. Set=1 after race exit; triggers FUN_00414740 (frontend setup) | 3 (1R, 2W) | Yes |
| 0x474C08   | DAT_00474c08   | `g_benchmarkImageLoaded`    | dword     | Benchmark mode TGA loaded flag. Set=1 after FPS screenshot loaded; cleared when drawn | 3 (1R, 2W) | Yes |
| 0x495248   | DAT_00495248   | `g_raceLaunchTrigger`       | dword     | Race launch trigger. Non-zero signals transition from MENU to RACE state | 5 (3R, 2W) | Yes |
| 0x49524C   | DAT_0049524c   | `g_raceLaunchMode`          | dword     | Race launch mode selector. Distinct from trigger; controls which path race init takes | 4 (2R, 2W) | Yes |

### Static Texture System (0x4C3CF0-0x4C3D00)

| Address    | Old Name       | New Name                       | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|--------------------------------|-----------|---------|-------|----------|
| 0x4C3CF0   | DAT_004c3cf0   | `g_staticTextureCount`         | uint      | Number of static textures loaded from static.hed/static.zip. Adjusted by -4 in non-splitscreen | 9 (2W, 7R) | Yes |
| 0x4C3CF4   | DAT_004c3cf4   | `g_splitscreenTextureOffset`   | dword     | Texture index offset for splitscreen mode (added when DAT_004c3d44==2) | 2 (1W, 1R) | Yes |
| 0x4C3CF8   | DAT_004c3cf8   | `g_staticTextureMipLevels`     | uint      | Mip level count from static.hed header; used to compute descriptor table offset | 8 (2W, 6R) | Yes |
| 0x4C3D00   | DAT_004c3d00   | `g_staticTextureDescTable`     | uint*     | Pointer to per-texture descriptor records within the static.hed allocation (16 bytes/mip per entry) | 9 (2W, 7R) | Yes |

### Frontend / Network Flow

| Address    | Old Name       | New Name                       | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|--------------------------------|-----------|---------|-------|----------|
| 0x4962C0   | DAT_004962c0   | `g_networkSessionActive`       | dword     | Network session state flag. Written by session create/join/lobby functions; read by race type menu and car selection | 14 (9W, 5R) | Yes |

### Rendering Pipeline -- Texture State

| Address    | Old Name       | New Name                       | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|--------------------------------|-----------|---------|-------|----------|
| 0x48DBAC   | DAT_0048dbac   | `g_perSlotTransparencyType`    | dword[32] | Per-texture-slot transparency type (0=opaque, 1=alpha test, 2=alpha blend, 3=clamp+blend) | 4 (3W, 1R) | Yes |
| 0x48DC30   | DAT_0048dc30   | `g_currentTransparencyMode`    | dword     | Currently active transparency mode on D3D device. Avoids redundant state changes | 2 (1R, 1W) | Yes |
| 0x48DC38   | DAT_0048dc38   | `g_currentTextureFilterMode`   | dword     | Current texture filter mode. Tracks last-set mode to skip redundant D3D calls | 2 (1R, 1W) | Yes |
| 0x48DC40   | DAT_0048dc40   | `g_texCacheDescArray`          | int*      | Texture cache descriptor array base. Indexed by texture slot to get transparency type, filter mode, and D3D handle offset | 4 (1W, 3R) | Yes |

### Rendering Pipeline -- Depth Sorting

| Address    | Old Name       | New Name                       | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|--------------------------------|-----------|---------|-------|----------|
| 0x4BF520   | DAT_004bf520   | `g_depthSortFreeListHead`      | ptr       | Linked-list head for depth sort node free pool. Reset to DAT_004bf6c4 each frame | 9 (5W, 4R) | Yes |
| 0x4C36F8   | DAT_004c36f8   | `g_depthSortActiveCount`       | dword     | Active depth-sorted translucent primitive count. Reset to 0 each frame | 10 (5W, 5R) | Yes |

### 3D Asset / Actor System

| Address    | Old Name       | New Name                       | Type      | Purpose | Xrefs | Renamed? |
|------------|----------------|--------------------------------|-----------|---------|-------|----------|
| 0x482DCC   | DAT_00482dcc   | `g_brakedMeshVertexBuffer`     | ptr       | BRAKED mesh vertex buffer allocation (DAT_004aaf00 * 0x2E0 bytes). Used for brake light overlay rendering | 4 (1W, 3R) | Yes |
| 0x4AEE10   | DAT_004aee10   | `g_meshEntryTable`             | int       | MODELS.DAT mesh entry table pointer. Indexed by model ID to get mesh data offset and render type | 11 (1W, 10R) | Yes |
| 0x4A31AC   | DAT_004a31ac   | `g_perViewActorState`          | byte[]    | Per-view actor state base. Stride 0x1900 per view, contains 100 actor slots with function pointers and flags | 6 (DATA refs) | Yes |
| 0x4A63D4   | DAT_004a63d4   | `g_currentViewIndex`           | int       | Current viewport index being rendered (0-3). Set before iterating actor render callbacks | 6+ | Yes |

---

## Items Not Renamed (Insufficient Evidence or No Direct Xrefs)

| Address Range          | Catalog Notes | Status |
|------------------------|---------------|--------|
| 0x4C3CEC               | Adjacent to g_gameState | No xrefs found. Likely padding or unused field |
| 0x473C6C+4..+60        | Near track strip C | No xrefs at +4/+8/+12 offsets. Likely accessed via pointer arithmetic from 0x473C6C |
| 0x46A080-0x46BB00      | Environment object table | No direct xrefs to sampled addresses. Accessed via computed pointer from environment loader |

---

## Analysis Notes

### Rendering State Cluster (0x48DBA0-0x48DC48)

This block forms a coherent "texture/render state" struct:
- 0x48DBA0: `g_raceRenderActive` (already named)
- 0x48DBA8: `g_renderGlobalsInit` (already named)
- 0x48DBAC: `g_perSlotTransparencyType` -- 32-entry dword array (0x80 bytes)
- 0x48DC2C: end of transparency array
- 0x48DC30: `g_currentTransparencyMode`
- 0x48DC34: `g_boundTextureSlot` (already named)
- 0x48DC38: `g_currentTextureFilterMode`
- 0x48DC3C: `g_texCacheControlBlock` (already named)
- 0x48DC40: `g_texCacheDescArray`
- 0x48DC44: already named via catalog
- 0x48DC48: `g_billboardYOffset` (already named)

### Depth Sort System (0x4BF520-0x4C36F8)

The depth sorting uses a 4096-bucket array at 0x4BF6C8 (already named `g_depthSortBuckets`).
Each bucket is a 4-byte pointer (linked list head). The free list starts at 0x4BF520 and
the active count at 0x4C36F8. Every frame, `FUN_0043e2c0` clears all 4096 buckets and
resets the free list.

### Static Texture Loading (0x4C3CF0-0x4C3D00)

The three new globals form the static texture system state loaded from `static.hed`/`static.zip`.
`g_staticTextureCount` is the total texture count, `g_staticTextureMipLevels` stores the
mip chain depth, and `g_staticTextureDescTable` points to per-texture dimension/format records.
In splitscreen mode (g_splitscreenPlayerCount==2), the count is not decremented by 4 and
`g_splitscreenTextureOffset` is applied when looking up per-model textures.
