---
batch: 34
area: frontend_display_modes
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x004265c0, 0x00426600, 0x00425500, 0x00426000, 0x00426200, 0x00425a00
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — frontend display-mode list + selection (T6)

## Summary

- Functions analyzed: UpdateFrontendDisplayModeSelection, CreateFrontendDisplayModeButton, BeginFrontendDisplayModePreviewLayout, RestoreFrontendDisplayModePreviewLayout, ReleaseFrontendDisplayModeButtons, ResetFrontendSelectionState, InitializeFrontendDisplayModeArrows, RenderFrontendDisplayModeHighlight, RenderFrontendUiRects, MoveFrontendSpriteRect, RebuildFrontendButtonSurface, CreateFrontendMenuRectEntry
- Unnamed DAT_* targeted: 18+
- Already-named neighbors noted: g_connBrowserListOriginX, g_connBrowserListOriginY, g_connBrowserListRowStride (PROVISIONAL)
- Proposals — high: 12
- Proposals — medium: 5
- Proposals — comment-only: 2

## Methodology

Cluster C.5 from W1-E: 0x00499c78..0x00499cc0 (76 B) — accessed by 5 frontend display-mode functions. Re-walked all xrefs and used the `MOV [ESI + 0x499cXX]` and `CMP [ECX + 0x499cXX]` patterns to assign struct-field semantics. The `*4` stride patterns reveal entry-array origins. The `TEST byte [...],0x2/0x4` patterns reveal flag fields.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00499c80 | u32 (offset) | `g_displayModeBtnEntry_offset_visualPtr` | high | `CMP [ECX + 0x499c80],ESI` in UpdateFrontendDisplayModeSelection 3x — DisplayModeBtn entry struct field offset (10 refs) | `(none)` |
| 0x00499c84 | u32 (offset) | `g_displayModeBtnEntry_offset_glyphPtr` | high | `CMP [ESI+0x4], EAX` adjacent +4 in UpdateFrontendDisplayModeSelection (9 refs) — 2nd field | `(none)` |
| 0x00499c88 | u32* | `g_displayModeBtnListHead` | high | UpdateFrontendDisplayModeSelection / CreateFrontendDisplayModeButton / ResetFrontendSelectionState head ptr (14 refs) | `(none)` |
| 0x00499c90 | u32 (offset) | `g_displayModeBtnEntry_offset_xPos` | medium | `[ESI + 0x499c90]` 3x in RenderFrontendDisplayModeHighlight (5 refs) — x-pos field | `(none)` |
| 0x00499c94 | u32 (offset) | `g_displayModeBtnEntry_offset_yPos` | medium | `[ESI + 0x499c94]` in RenderFrontendDisplayModeHighlight (6 refs) — y-pos field | `(none)` |
| 0x00499c9c | u32 (offset) | `g_displayModeBtnEntry_offset_widthHeight` | medium | Same pattern (5 refs); InitializeFrontendDisplayModeArrows reads | `(none)` |
| 0x00499ca4 | u8 (offset) | `g_displayModeBtnEntry_offset_flags` | high | `TEST [EAX+0x499ca4],0x2`, `TEST [ECX+0x499ca4],0x2`, `MOV [ESI+0x499ca4],0x1/0x5` — flag byte (7 refs) | `(none)` |
| 0x00499cac | u32 (offset) | `g_displayModeBtnEntry_offset_nextPtr` | medium | Linked-list next ptr (7 refs); CreateFrontendDisplayModeButton chains entries | `(none)` |
| 0x00499cb0 | u32 (offset) | `g_displayModeBtnEntry_offset_prevPtr` | medium | (6 refs); paired with nextPtr | `(none)` |
| 0x00499cbc | u32[N] | `g_displayModeBtnEntryTable` | high | `[ECX*4 + 0x499cbc]` in UpdateFrontendDisplayModeSelection (12 refs); CMP `[EAX],-0x1` sentinel — entry index lookup | `(none)` |
| 0x00499cd8 | u32 | `g_displayModeBtnSelectedIndex` | high | CreateFrontendDisplayModeButton writes 1/5; UpdateFrontendDisplayModeSelection reads (7 refs) | `(none)` |
| 0x00499cb4 | u32 | `g_displayModeBtnHighlight_PROVISIONAL` | medium | (4 refs) | `(none)` |
| 0x00499cb8 | u32 | `g_displayModeBtnAnimPhase_PROVISIONAL` | medium | (4 refs) | `(none)` |
| 0x00499cc0 | u32 | `g_displayModeBtnSurfaceCount_PROVISIONAL` | medium | (4 refs) | `(none)` |
| 0x00499cc8 | u32 | `g_displayModeBtnAcceptFlag` | medium | CreateFrontendDisplayModeButton writes (4 refs) | `(none)` |
| 0x00499cd0 | u32 | `g_displayModeBtnRebuildPending` | low | (4 refs) | `(none)` |
| 0x00499cdc | u32 | `g_displayModeBtnCacheSlotMask` | low | (4 refs) — UpdateFrontendDisplayModeSelection | `(none)` |
| 0x00499cf0 | u32 | `g_displayModeBtnInputMutex_PROVISIONAL` | low | (4 refs) | `(none)` |
| 0x0049a978 | u32 | `g_frontendDisplayModePreviewCount` | medium | CreateFrontendDisplayModeButton+UpdateFrontendDisplayModeSelection+ResetFrontendSelectionState+AdvanceFrontendTickAndCheckReady (6 refs) | `(none)` |
| 0x0049a980 | u32 | `g_frontendDisplayModePreviewActiveIndex` | medium | RenderFrontendUiRects + BeginFrontendDisplayModePreviewLayout + RestoreFrontendDisplayModePreviewLayout (5 refs) | `(none)` |
| 0x0049a97c | u32 | `g_frontendDisplayModePreviewBaseW_PROVISIONAL` | low | (4 refs) | `(none)` |
| 0x0049a988 | u32 | `g_frontendDisplayModePreviewBaseH_PROVISIONAL` | low | (4 refs) | `(none)` |
| 0x0049a984 | u32 | `g_frontendDisplayModePreviewBaseX_PROVISIONAL` | low | (3 refs) | `(none)` |
| 0x0049a99c | u32 | `g_frontendDisplayModePreviewBaseY_PROVISIONAL` | low | (3 refs) | `(none)` |
| 0x00498704 | u32 | `g_frontendOverlayRectCursor` | medium | UpdateFrontendDisplayModeSelection (4 refs) | `(none)` |
| 0x00498708 | u32 | `g_frontendOverlayRectCount` | medium | RenderFrontendUiRects + BeginFrontendDisplayModePreviewLayout + ResetFrontendSelectionState + RestoreFrontendDisplayModePreviewLayout (6 refs) | `(none)` |
| 0x00498718 | u32 | `g_frontendOverlayRectArrayHead` | medium | FlushFrontendSpriteBlits + ResetFrontendOverlayState + QueueFrontendOverlayRect (6 refs) | `(none)` |
| 0x00498720 | u32 | `g_frontendOverlayRectArrayCount` | medium | FlushFrontendSpriteBlits + ResetFrontendOverlayState (8 refs) | `(none)` |
| 0x00498700 | u32 | `g_frontendOverlayRectArrayTail` | medium | UpdateFrontendDisplayModeSelection + RenderFrontendDisplayModeHighlight (5 refs) | `(none)` |
| 0x004969dc | u32 | `g_pendingNetMsgPayloadPtr` | medium | (dup) — see batch 28 | `(none)` |

## Key discoveries

- Two-level frontend menu structure: a **linked-list of DisplayModeBtn entries** (0x00499c80..0x499cd8 range = field offsets of one entry) plus a **lookup table indexed by entry ID** at 0x00499cbc (with -0x1 sentinel for end). Total entry struct = ~0x50 bytes.
- The DisplayModeBtn entries form an array-of-pointers selected via `[idx*4 + 0x499cbc]`, then walked as nodes (next/prev). Wave 2 should type as struct `FrontendDisplayModeBtn`.
- 0x0049a978..0x0049a99c is a SEPARATE smaller "active preview" descriptor (count, active index, base x/y/w/h, ~0x28 bytes).
- 0x00498700..0x00498724 is a third frontend cluster — **overlay-rect ring buffer** for sprite blits / overlays. ResetFrontendOverlayState clears all of these.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004969d9 / dc | already in batch 28 | net msg |
| 0x0049a988/c | preview struct field — same as above | already covered |
| 0x0049b680 | u32 — FlushFrontendSpriteBlits+QueueFrontendSpriteBlit+ResetFrontendOverlayState (5 refs) — sprite-blit queue head | T7 frontend |

## TODO impact

- No direct closures. Display-mode/preview UI is largely benign frontend rendering; surface for any future investigations into frontend-button rendering pipeline (Phase 6 parity).
