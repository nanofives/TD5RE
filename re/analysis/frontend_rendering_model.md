# TD5 Frontend Rendering Model — Behavior-Level RE

RE target: `TD5_d3d.exe` (image base `0x00400000`). All addresses are Ghidra VAs.
Source: literal decompilation + memory/symbol facts only. Decompiled every function in
sections 3/4/5 of `frontend_call_graph_closure.md`, plus the globals in section 10.

## Conventions established by decompilation (apply throughout)

- **Surface vtable offsets** (DirectDrawSurface*, COM-style first member = `*surface`):
  - `+0x08` = Release; `+0x14` = `Blt` (used for COLORFILL with flag `0x400`=DDBLT_COLORFILL);
    `+0x1c` = `BltFast` (src→dst rect copy; flag `0x10`=wait, `0x11`=wait+SRCCOLORKEY);
    `+0x34` = `Restore` (busy-loop on `0x887606ec`); `+0x64` (=`100`) = `Lock`;
    `+0x6c` = `Restore`(alt); `+0x74` = `SetColorKey` (sub-cmd `8`=DDCKEY_SRCBLT);
    `+0x80` = `Unlock`.
- **16bpp pixel packing** (used by every fill helper). Input `param` is 0x00RRGGBB (24-bit):
  - 555 mode (`g_frontendSurfaceBitDepth==0xf`): `((param>>3 & 0x1f0000 | param&0xf800)>>3 | param&0xf8)>>3`.
  - 565 mode (`==0x10`): same but the first `>>3` becomes `>>2`.
- **g_frontendSurfaceBitDepth** = `0xf` (15) for RGB555, `0x10` (16) for RGB565. Selects every
  pixel path; `!=` either value → fill is a no-op (color left 0).
- **Three font glyph atlases** (resolved below): SMALL 12×12 / 21-col, BODY 12×12 / 21-col,
  MENU(large) 36×36 / 7-col, LOCALIZED(wide) 24×24 / 10-col.

---

# PART 1 — THE PER-FRAME PIPELINE

`FlushFrontendSpriteBlits` `0x00425540` `__stdcall void(void)` is the per-frame compositor.
The LOOP (`RunFrontendDisplayLoop 0x00414b50`) calls it AFTER the active screen-fn and after
`RenderFrontendUiRects`. EXACT ordered sequence each frame:

1. **Overlay-rect double-buffer drain** (the queued sprite/overlay rects):
   - `piVar3 = &g_frontendOverlayRectArrayCount_PROVISIONAL (0x00498720) + g_frontendFrameToggle*0x410`.
     `g_frontendFrameToggle` `0x004951fc` selects one of two `0x410`-byte (1040-byte) banks.
   - **Defer/skip gate**: if `g_frontendOverlayRectCursor_PROVISIONAL (0x00498704) == 0` → drain;
     else decrement it and skip the drain entirely (1-frame deferred-restore latch; set to `2`
     by `DeferFrontendBackgroundRestore 0x004258b0`).
   - Loop up to **`0x40` (64)** records, stride `4` dwords (16 bytes): stop at first record
     whose dword[0]==`-1` (sentinel). Each → `Copy16BitSurfaceRect(rec[0], rec[1],
     g_primaryWorkSurface, rec, 0x10)` (plain copy, NO color-key) into `g_primaryWorkSurface`
     (`0x00496260`).
2. **Reset the active bank head**: `*piVar4 = -1` (writes sentinel at bank base).
3. `UpdateExtrasGalleryDisplay()` — **DECOUPLED draw #1** (unconditional; see Part 2).
4. `RenderFrontendDisplayModeHighlight()` — **DECOUPLED draw #2** (unconditional; see Part 2).
5. **Cursor-tracked sprite-list blit** (the on-screen sprite/button list):
   - Walk `DAT_00497ad4 (0x00497ad4)` stride `0xc` dwords (48 bytes), up to `0x40` (64) entries,
     stop at first entry whose `[3]==-1`.
   - For each entry that has a non-zero surface (`[9]`), scan the surface registry
     (`&g_frontendSurfaceRegistryTail 0x004951d0` step 2 dwords, end `<0x4951d4`) to match
     `[9]==surface_ptr && [10]==registry_id` (validates the surface is still live):
     - Re-prime the active overlay bank record from the sprite entry (`*piVar4 = [-1]`, etc.),
     - `surface->SetColorKey(+0x74)(surf, 8, entry+7)` (DDCKEY_SRCBLT from entry’s key field),
     - `Copy16BitSurfaceRect(rec[0], rec[1], entry->surface, entry+3, 0x11)` (**color-keyed** copy
       into `g_primaryWorkSurface`).
6. **Resets**: `*piVar4 = -1`; `g_frontendSpriteBlitCursor_PROVISIONAL = 0xffffffff`;
   `g_frontendOverlayRectArrayHead_PROVISIONAL (0x00498718) = 0`; `DAT_0049b680 = 0`
   (the sprite-blit count consumed by `QueueFrontendSpriteBlit`).

So one frame = (drain queued overlay rects, plain-copy) → gallery cross-fade → selection edge
bars → (drain queued sprites, color-keyed copy). Both the overlay drain and the sprite drain
target `g_primaryWorkSurface`. Present to the visible surface is done by the LOOP afterward
(`PresentFrontendBufferSoftware` or DXDraw::Flip; see Part 3 present helpers).

---

# PART 2 — EVERY DECOUPLED DRAW (the prior-pass blind spot)

These run from the per-frame flush (or its chain), read state primed by screen-fns, and are not
called by any screen-fn directly. Count of decoupled draws documented: **4** (gallery display,
its slideshow-advance, its cross-fade blend, and the selection highlight) — matching the closure's
"5 decoupled" minus `QueueFrontendOverlayRect`/UI walk which ARE LOOP-driven.

### UpdateExtrasGalleryDisplay `0x0040d830` `__stdcall void(void)`  (DECOUPLED)
Reads: `g_extrasGalleryCrossFadePhase (0x?)`, `g_extrasGallerySlideSurfaces (0x0048f2d4)` (gate:
must be non-zero or whole fn no-ops), `g_frontendScreenTransitionFlag`, `g_attractCdTrackCandidate`,
`g_extrasGalleryPreviousSlideIndex_PROVISIONAL`, `g_extrasGalleryEnabledFlag_PROVISIONAL`,
`g_extrasGalleryCurrentSlidePtr`, slide pos `g_extrasGallerySlideX/Y`.
Two modes by `g_frontendScreenTransitionFlag`:
- **==2 (band-gallery active, from `LoadExtrasBandGalleryImages`)**: When the CD track’s band
  index changes (LUT compare `*(char*)(g_attractCdTrackCandidate + 0x465e4c)` vs prev), it picks
  the new slide `(&g_extrasGallerySlideSurfaces)[ LUT[track] ]`, sets dest `(0x76,0x8c)` = (118,140),
  phase `0x100` (256), and erases the previous slide rect via `g_primaryWorkSurface->BltFast(+0x1c)`.
  Then halves the phase and derives two blend weights `uVar1,uVar3` (each clamped ≤0x20=32) from
  the phase band; if `g_extrasGalleryEnabledFlag_PROVISIONAL!=0` calls `CrossFade16BitSurfaces`,
  else plain `Copy16BitSurfaceRect` at the slide pos. Phase decremented toward floor `-0x40` (-64).
  **Band→slide LUT @ 0x00465e4c** (12 bytes read): `01 03 04 04 02 00 00 01 03 04 04 04` — each CD
  track index → one of 5 band slide surfaces (0..4).
- **!=2 path (extras-screen idle)**: if disabled or transition!=0 sets phase `-0x10`; else halves
  phase, derives weights (`0x18`/`0x14` base), cross-fades the current slide, and **at phase==-0x18
  (-24) calls `AdvanceExtrasGallerySlideshow()`**.
Draws to: `g_primaryWorkSurface` (via CrossFade/Copy). Position `(0x76,0x8c)` for the band path;
random for the slideshow path.

### AdvanceExtrasGallerySlideshow `0x0040d750` `__stdcall uint(void)`  (DECOUPLED, gallery)
Picks a random next slide `(&g_extrasGallerySlideSurfaces)[rand % g_extrasGallerySlideCount]`
(loops until != current), Locks it (`+0x64`), Unlocks (`+0x80`), sets new random position
`g_extrasGallerySlideX = rand%(500-w)+0x8c`, `g_extrasGallerySlideY = rand%0x150 + 0x54`, phase
`0x100` (256). On lock-fail logs `SNK_StartRandomFa...`. No direct pixel write; sets up the next
fade for `UpdateExtrasGalleryDisplay`.

### CrossFade16BitSurfaces `0x0040d190` `__cdecl void(uint a, uint b, int x, int y, int* src)` (gallery helper)
Alpha cross-fade of a 16bpp `src` slide into `g_primaryWorkSurface`, reading the background from
`g_frontendBackSurfacePtr (0x00495220)`. Weights `a,b` clamped ≤`0x20` (32). Locks all three
surfaces (`+0x64`), MMX/pmullw blend per pixel with separate 555 (`g_frontendSurfaceBitDepth==0xf`)
and 565 (`==0x10`) code paths (mask `0x7c0`/`0xf800` constants), pixel `==0` treated as transparent
(`-(ushort)(c==0)` mask preserves background). Unlocks. On lock-fail → `Msg("Lock_*_failed_in_SNK_CrossTr...")`.

### RenderFrontendDisplayModeHighlight `0x004263e0` `__stdcall void(void)`  (DECOUPLED)
Gate: `g_frontendOverlayRectArrayTail_PROVISIONAL != -1` (selected rect index) AND
`g_frontendCursorOverlayHidden == 0`. Draws **4 edge bars** of color `0xc000` around the selected
menu rect `g_connBrowserListOriginX_PROVISIONAL[tail]` into `g_frontendBackSurfacePtr`
(NOT the work surface) via `BltColorFillToSurface`:
- top:   `(x+0x14, y+4, w-0x28, 2)`
- bottom:`(x+0x14, end_y-6, w-0x28, 2)`
- left:  `(x+0x14, y+4, 2, h-10)`
- right: `(end_x-0x16, y+4, 2, h-10)`
(`0x14`=20, `0x28`=40 insets; bar thickness 2). This is the hover/selection outline.

---

# PART 3 — DRAW / TEXT / FILL / FADE HELPERS (section 4)

## Color-fill family (all share the 16bpp pack + Blt `+0x14` COLORFILL `0x400`)
| addr | name | target surface | rect |
|---|---|---|---|
| `0x00423db0` | ClearBackbufferWithColor | `g_primaryWorkSurface` | whole surface (rect ptr 0) |
| `0x00423ed0` | FillPrimaryFrontendRect | `g_primaryWorkSurface` | (x,y,w,h) |
| `0x00423f90` | FillSurfaceRectWithColor | `g_secondaryWorkSurface` | (x,y,w,h) |
| `0x00424050` | BltColorFillToSurface | **arbitrary `param_6` surface** | (x,y,w,h) — used by highlight bars & button frames |
| `0x00423e40` | LockSecondaryFrontendSurfaceFillColor | `g_secondaryWorkSurface` | whole surface |
| `0x00424d90` | FillPrimaryFrontendScanline | `g_primaryWorkSurface` | full-width 1-px row at y |

All take a 24-bit RGB `color` arg; convert to the surface’s 16bpp. `BltColorFillToSurface` is the
generic primitive (the highlight bars, the 9-slice button frame edges, panels).

## Text/string family
All blit glyph cells from a font atlas surface via `BltFast (+0x1c)` flag `0x11` (color-keyed).
Glyph index = `char - 0x20` (space-origin). Advance/kerning tables are per-char byte arrays.

| addr | name | font surface | glyph cell | atlas cols | notes |
|---|---|---|---|---|---|
| `0x00424110` | DrawFrontendFontStringPrimary | `g_bodyTextFontSurface (0x49626c)` | 12×12 (`0xc`) | 21 (`0x15`) | x+8 offset; advance `(&g_smallFontAdvance)[c]` |
| `0x004241e0` | DrawFrontendFontStringSecondary | `g_bodyTextFontSurface` | 12×12 | 21 | → `g_secondaryWorkSurface`; y+8; returns measured width+0x18 |
| `0x004242b0` | DrawFrontendLocalizedStringPrimary | `g_bodyTextFontSurface` | **24×24 (`0x18`)** | **10** | only when LANGUAGE.DLL flag (`SNK_LangDLL_exref[8]==0x30`); else delegates to body 12×12; advance LUT `PTR_DAT_004660c8` |
| `0x00424390` | DrawFrontendLocalizedStringSecondary | `g_secondaryWorkSurface` | 24×24 | 10 | same gate; else delegates to FontStringSecondary |
| `0x00424470` | DrawFrontendFontStringToSurface | `g_bodyTextFontSurface` | 12×12 | 21 | target = `param_4` (any surface); y-offset LUT `(&g_smallFontYOffset)[c]` |
| `0x00424560` | DrawFrontendLocalizedStringToSurface | (24×24 path) | 24×24 | 10 | gate; else → DrawFrontendFontStringToSurface |
| `0x00424660` | DrawFrontendSmallFontStringToSurface | **`g_smallFontSurface (0x49627c)`** | 12×12 | 21 | distinct atlas surface from body |
| `0x00424740` | DrawFrontendClippedStringToSurface | `g_smallFontSurface` + scroll-arrow sprite `g_browserScrollIndicatorSprite_PROVISIONAL (0x496288)` for `c<0x20` | 12×12, clip width `param_5` | 21 | per-glyph horizontal clip; control chars (<0x20) draw scroll-indicator cells |
| `0x004248e0` | DrawFrontendWrappedStringLine | (uses DrawFrontendClippedStringToSurface) | — | — | word-wrap: tokenizes on space into 256-byte buf, measures, emits clipped lines |
| `0x00412d50` | MeasureOrDrawFrontendFontString | **`g_menuFontSurface (0x496278)`** | **36×36 (`0x24`)** | **7** | LARGE/menu font; dual-mode (param_4==0 → measure-only). advance LUT `g_largeFontAdvance` |

## Measure/center helpers (no draw)
| addr | name | behavior |
|---|---|---|
| `0x00424890` | MeasureOrCenterFrontendString | sums `g_smallFontAdvance[c]` (12 for ctrl); param_3!=0 → centered X |
| `0x00424a50` | MeasureOrCenterFrontendLocalizedString | localized advance LUT when LANG flag, else small advance; centers |

## Fade family (dither-ramp scanline bars; all → `g_primaryWorkSurface`)
- `BuildFrontendDitherOffsetTable 0x00411710` (INIT): fills `g_frontendDitherRampLut (0x00494bc0)`,
  1024 (`0x400`) bytes. For index = `(hi5<<5)|lo5`, value = `lo5` adjusted ±1 toward `hi5`
  (lo==hi → lo; lo>hi → lo-1; lo<hi → lo+1). This is the per-channel dither-step ramp.
- `InitFrontendFadeColor 0x00411750`: `g_frontendFadeScanCursor=0; g_frontendFadeActive=1;
  _g_frontendFadeChannelR (packed R/G/B) = color>>3 & 0x1f1f1f` (the fade target color, 5-bit/chan).
- `ResetFrontendFadeState 0x00411a50`: cursor=0, channels=0, active=1 (fade-to-black setup).
- `RenderFrontendFadeEffect 0x00411780` (fade-IN/to-color): Locks `g_primaryWorkSurface`, processes
  a moving **`0x40` (64)-row band** `[cursor-0x40 .. cursor]`; each pixel’s 5-bit channels are
  remapped through `g_frontendDitherRampLut[(fadeChan<<5)|chan]` (separate 555/565 code), then
  `Copy16BitSurfaceRect(0, top, g_primaryWorkSurface, …, 0x10)` flushes the band. `cursor += 2`
  each frame; when `cursor-0x40 > canvasH` → `g_frontendFadeActive=0`.
- `RenderFrontendFadeOutEffect 0x00411a70` (fade-OUT/cross): same band mechanic but blends
  `g_primaryWorkSurface` toward `g_secondaryWorkSurface` content through the same dither LUT (Locks
  both). `cursor += 2`; finish when band passes bottom.
Fade mechanic = a 64-row dither-stepped wipe band that scans top→bottom 2 rows/frame.

## Button-composition helpers
- `DrawFrontendButtonBackground 0x00425b60` `(bx,by,bw,bh,state,…)`: composes a **9-slice button
  frame** onto button surface `bx` from source page `DAT_00496268` (the button-frame atlas) using
  repeated `BltFast (+0x1c)` calls — corners, top/bottom edges (4-px step loops), left/right edges,
  and center fill. `state*0x20`/`state*0xc` select the source-cell row → normal/pressed/preview
  variant. The button BACKGROUND only (no label, no arrows).
- `CreateFrontendDisplayModeButton 0x00425de0` `(label, x, y, w, h, user_data)`: the full button
  factory. (1) `CreateTrackedFrontendSurface(w, h*2)` (h*2 when not preview — bakes BOTH normal &
  pressed states stacked). (2) `BltColorFillToSurface(0,…)` clears. (3) Draws frame: preview mode
  (`DAT_0049b694!=0`) → one `DrawFrontendButtonBackground(…,state=2)`; normal → two calls (state 1
  at y=0 = highlighted, state 0 at y=h = idle). (4) If `label!=0` and it fits width: centers via
  `MeasureOrCenterFrontendLocalizedString` and bakes the text twice (normal+pressed) with
  `DrawFrontendLocalizedStringToSurface`; preview mode instead half-brights the surface (MMX shift,
  `0x7bcf`/`0x3def` masks + add `0x2104`/`0x1084`). (5) Finds first free slot in
  `g_connBrowserListOriginX_PROVISIONAL[]` (sentinel scan), fills the FrontendButtonSlot fields,
  sets `flags=1` (active) or `flags=5` (active+preview), registers the surface id.
- `CreateFrontendDisplayModePreviewButton 0x004260e0`: sets `DAT_0049b694=1`, calls
  CreateFrontendDisplayModeButton, clears flag — i.e. the preview-image variant.
- `RebuildFrontendButtonSurface 0x00426120` `(index)`: re-bakes the existing slot’s surface in place
  (same frame+label sequence) for label/state/pixel-format changes; resets `select_progress`.
- `InitializeFrontendDisplayModeArrows 0x00426260` `(index, right_flag)`: blits ◄/► arrow sprites
  from `g_browserSelectionBarSprite_PROVISIONAL (0x00496284)` onto the slot’s surface (left arrow at
  `sentinel+7`, right arrow at `width-0x12`; source cells `(0,0,0xc,9)` left / `(0,0x12,0xc,0x1b)`
  right; flag `0x11` color-keyed) and **sets the slot’s `flags |= 2`** (two-axis-nav = arrow-capable;
  consumed by UpdateFrontendDisplayModeSelection’s ◄►/center logic).
- `ReleaseFrontendDisplayModeButtons 0x00426390`: walks the slot table; for slots with `flags&1`
  releases the tracked surface; sets all sentinels `-1`; resets `g_frontendButtonIndex`,
  `g_frontendEscKeyButtonIndex=-1`.
- `BeginFrontendDisplayModePreviewLayout 0x004264e0` / `RestoreFrontendDisplayModePreviewLayout
  0x00426540`: snapshot/restore the entire `0xd00` (3328)-byte button table to/from
  `g_frontendDisplayModePreviewActiveIndex_PROVISIONAL` (the preview overlay layer) so a preview
  screen can lay out its own buttons then restore the menu.
- `CreateFrontendMenuRectEntry 0x004258f0`: adds a non-button hit/draw rect to the slot table
  (`flags=0`, no surface alloc here — surface is `param_8`).
- `MoveFrontendSpriteRect 0x004259d0`: repositions slot `param_1` preserving its w/h.
- `RenderPositionerGlyphStrip 0x00414f40` (debug, SCR1): emits `QueueFrontendOverlayRect` records of
  36×36 (`0x24`) menu-font glyphs from `g_menuFontSurface` plus 4-px separators — a glyph ruler for
  the element-positioner tool.

## Present / copy helpers (Part 3 cont.)
| addr | name | direction (BltFast `+0x1c` or Copy16BitSurfaceRect) |
|---|---|---|
| `0x004251a0` | Copy16BitSurfaceRect | core 16bpp rect blit; flag&1 → color-key vs `src[? ]`(local_50). If `g_frontendHardwareFlipEnabled_Alt_PROVISIONAL!=0` it just forwards to `g_frontendBackSurfacePtr->BltFast`; else Locks back+src and does a CPU 16-bit copy (keyed or plain). |
| `0x00425360` | PresentFrontendBufferSoftware | SW path: Locks `g_frontendBackSurfacePtr` + the DDraw front buffer (`dd_exref+4`), CPU-copies `canvasW×canvasH` to the client-origin offset cached by UpdateFrontendClientOrigin. |
| `0x00424b80` | BlitFrontendCachedRect | `Copy16BitSurfaceRect(x,y,g_primaryWorkSurface,…,0x10)` — restores a cached bg rect into the work surface (lost-surface redraw). |
| `0x00424af0` | PresentPrimaryFrontendBufferViaCopy | full primary → present via Copy16BitSurfaceRect(0x10). |
| `0x00424b30` | CopyPrimaryFrontendBufferToSecondary | `g_secondaryWorkSurface->BltFast(prim, 0x10)`. |
| `0x00424bc0` | CopyPrimaryFrontendRectToSecondary | rect prim→secondary BltFast 0x10. |
| `0x00424c10` | PresentSecondaryFrontendRectViaCopy | Copy16BitSurfaceRect from secondary. |
| `0x00424c50` | BlitSecondaryFrontendRectToPrimary | `g_primaryWorkSurface->BltFast(secondary, 0x10)`. |
| `0x00424ca0` | PresentPrimaryFrontendBuffer | `g_primaryWorkSurface->BltFast(g_frontendBackSurfacePtr, 0x10)`. |
| `0x00424cf0` | PresentSecondaryFrontendRect | `g_secondaryWorkSurface->BltFast(g_frontendBackSurfacePtr,…)`. |
| `0x00424d40` | PresentPrimaryFrontendRect | `g_primaryWorkSurface->BltFast(g_frontendBackSurfacePtr,…)`. |
| `0x00424e40` | InitializeFrontendPresentationState | INIT: clears primary, detects HW-flip caps (`dd_exref+0xf40 & 0x10` → `g_frontendHardwareFlipEnabled`), allocates `g_frontendBackSurfacePtr` if no HW flip; warm-up flip loop; CPUID/RDTSC MHz benchmark; sets `g_extrasGalleryEnabledFlag_PROVISIONAL` from CPUID feature bit 23. |
| `0x004258b0` | DeferFrontendBackgroundRestore | sets `g_frontendOverlayRectCursor_PROVISIONAL=2` (skip 1 overlay-drain frame). |

---

# PART 4 — ASSET / SURFACE LOADERS (section 5)

Common decode core (loaders `0x412030/0x4122f0/0x4125b0/0x4127b0`): `OpenArchiveFileForRead(name,
zip)` → `DX::Allocate(0x1d4c00)` conversion buffer → fill `Image_exref` DXIMAGELINE with TGA decode
params (`+0x2c=5`, masks `g_tgaDecodeRedMask/Green/Blue`) → `DX::ImageProTGA` → free file → create
target surface, Lock(`+0x64`), copy decoded pixels row-by-row, Unlock(`+0x80`), `SetColorKey(+0x74,
8)`, `DX::DeAllocate`. On any failure → `Msg(...)` + `_g_tgaLoadFailureLatch=1`, returns 0.

| addr | name | archive | dest surface | colorkey / special |
|---|---|---|---|---|
| `0x00412030` | LoadFrontendTgaSurfaceFromArchive | (name, zip) args | NEW tracked surface (w×h from TGA hdr `+0x52/+0x54`) | sets SRCBLT colorkey (sub-cmd 8). |
| `0x004122f0` | LoadTgaToFrontendSurfaceFromArchive | (name, zip) | NEW tracked surface | **substitutes pixel 0→1** so value-0 stays the transparent key but no opaque pixel collides; sets colorkey. |
| `0x004125b0` | LoadTgaToFrontendSurface16bpp | (name, zip) | `g_primaryWorkSurface` (clipped to canvas W/H) | decode straight into the work surface (full-screen background). |
| `0x004127b0` | LoadTgaToFrontendSurface16bppVariant | (name, zip) | `g_secondaryWorkSurface` | same as 0x4125b0 but secondary. |
| `0x004129b0` | RenderTgaToFrontendSurface | in-place on `param_1` | (locked) | **inverts** every pixel: each 5-bit channel → `0x1f-ch` (555/565 shift) — used for a negative/flash effect, NOT a loader despite name. |
| `0x00412b00` | SetSurfaceColorKeyFromRGB | — | `param_1` | packs RGB→16bpp and `SetColorKey(+0x74,8)`. |
| `0x00412b90` | RenderTgaWithColorKeyToSurface | in-place on `param_1` | (locked) | **half-bright dim with colorkey**: pixels != key are shifted-right (halved) per channel + value `+6` clamp — dim/disabled overlay. |
| `0x00414640` | LoadFrontendSoundEffects | `Front End\Sounds\Sounds.zip` | DXSound buffers 1..10 | loads ping1-3/Crash1/Whoosh/Uh_Oh into SFX slots (not a surface loader). |
| `0x0040d590` | LoadExtrasGalleryImageSurfaces | `Front End\Extras\Extras.zip` (pic1..pic5.tga) | `g_extrasGallerySlideSurfaces[0..4] (0x48f2d4..)` | gated on `g_extrasGalleryEnabledFlag_PROVISIONAL`; sets `slideCount=5`, phase=0, transition=0. |
| `0x0040d640` | ReleaseExtrasGalleryImageSurfaces | — | frees slide surfaces | sets transition=1, slideCount=0. |
| `0x0040d6a0` | LoadExtrasBandGalleryImages | Extras.zip (Fear_Factory/Gravity_Kills/Junkie_XL/KMFDM/PitchShifter.tga) | same `g_extrasGallerySlideSurfaces[0..4]` | uses 0x4122f0 (0→1 variant); sets **transition=2**, slideCount=5, prevIndex=-1 — primes the band-cover slideshow. |
| `0x00417dd2` | LoadFrontendExtrasGalleryResources | Mugshots.zip + Extras.zip | 26 surfaces `_DAT_004962e0..00496348` | dev-team mugshots (Gareth/Snake/MikeT/…) + Legals1-5; PTR-reached from SCR19 jump table. |

## Surface registry / tracking model
- `CreateTrackedFrontendSurface 0x00411f00 (w,h)`: `IDirectDraw::CreateSurface(+0x18)` with caps
  from `dd_exref+0x172c` (fallback caps `0x840` if `&0x20000000`); then registers in the parallel
  arrays `g_frontendSurfaceRegistryHead (0x004951c8, the surface ptr)` and `g_frontendSurfaceRegistryTail
  (0x004951cc, the id)`, stride 2 dwords, end `<0x4951d0`. New id from
  `g_trackedFrontendSurfaceListHead++` (skips 0). On full → `Msg("No_Free_Surfaces")`.
- `GetFrontendSurfaceRegistryId 0x00411e00`: linear scan head-array for ptr match → returns id.
- `ReleaseTrackedFrontendSurface 0x00411e30`: find slot, zero both arrays, `Restore(+0x34)`
  busy-loop, `Release(+8)`.
- `ReleaseTrackedFrontendSurfaces 0x00411e90`: release ALL registered surfaces (Restore+Release).
- `ClearFrontendSurfaceRegistry 0x00411de0`: zero both arrays (INIT).
The (ptr,id) pair is the contract the flush sprite-loop validates (`[9]==ptr && [10]==id`).

---

# PART 5 — THE GLOBAL CONTRACT (section 10, verified addresses)

| addr | symbol | written by | consumed by | role |
|---|---|---|---|---|
| `0x00496260` | g_primaryWorkSurface | INIT | flush, fills, blits, gallery | the composite work buffer (everything draws here) |
| `0x00496264` | g_secondaryWorkSurface | INIT | fade-out, secondary fills/copies | scratch/cross surface |
| `0x00495220` | g_frontendBackSurfacePtr | INIT (Create if no HW flip) | highlight bars, CrossFade bg, present | back/visible surface |
| `0x004951fc` | g_frontendFrameToggle | LOOP (^=1 per present) | flush (`*0x410` bank select) | overlay double-buffer toggle |
| `0x00498720` | g_frontendOverlayRectArrayCount_PROVISIONAL | QueueFrontendSpriteBlit bank | flush drain loop | overlay double-buffer base (2×`0x410`) |
| `0x00498718` | g_frontendOverlayRectArrayHead_PROVISIONAL | QueueFrontendOverlayRect (`++`) | flush (reset to 0) | static-rect array write cursor |
| `0x00498704` | g_frontendOverlayRectCursor_PROVISIONAL | DeferFrontendBackgroundRestore(=2) | flush (skip-drain countdown) | deferred-restore latch |
| (var) | g_frontendOverlayRectArrayTail_PROVISIONAL | UpdateFrontendDisplayModeSelection (hover) | RenderFrontendDisplayModeHighlight | which slot to outline |
| (var) | g_frontendButtonIndex | input/selection, ESC | RenderFrontendUiRects (offsets selected), highlight nav | active button |
| (var) | g_frontendEscKeyButtonIndex | screen fns / Release | LOOP ESC | ESC→button map |
| (var) | g_frontendButtonPressedFlag | LOOP/selection/input | screen fns | press latch |
| (var) | g_frontendCursorOverlayHidden | Activate/DeactivateFrontendCursorOverlay | RenderFrontendUiRects, highlight, selection | hide cursor & highlight |
| (var) | g_frontendMouseCursorEnabled | options/selection | LOOP cursor queue | mouse cursor on/off |
| `0x00499c78` | g_connBrowserListOriginX_PROVISIONAL[] | CreateFrontendDisplayModeButton / MenuRectEntry / Move | RenderFrontendUiRects, highlight, selection | **the menu-button slot table** — `FrontendButtonSlot` stride `0x34` (52 B / 0xd dwords): origin_x/y, end_x/y, sentinel_or_count_1/2, width, height, user_data, surface_ptr, surface_registry_id, flags(bit0=active,bit1=arrow/two-axis,bit2=disabled), select_progress(0..6 hover) |
| `0x0049a9a4` | DAT_0049a9a4[] | CreateFrontendDisplayModePreviewButton | RenderFrontendUiRects (preview pass, gate `g_frontendOverlayRectCount_PROVISIONAL==1`) | preview-button slot array |
| `0x00498f64` | DAT_00498f64[] | QueueFrontendOverlayRect / menu setup | RenderFrontendUiRects (static pass) | static rect/sprite slot array (stride 0xd dwords) |
| `0x00497ad4` | DAT_00497ad4[] | QueueFrontendSpriteBlit (stride 0xc) | flush final blit loop | cursor-tracked sprite list (per-frame, cleared each flush) |
| `0x004951c8`/`cc` | g_frontendSurfaceRegistryHead/Tail | CreateTrackedFrontendSurface | Release*, flush validate, GetId | (ptr,id) surface registry, stride 2 dwords |
| `0x0048f2d4` | g_extrasGallerySlideSurfaces (+4 more) | LoadExtras*GalleryImages | UpdateExtrasGalleryDisplay | 5 gallery slide surfaces |
| (var) | g_extrasGalleryCrossFadePhase | UpdateExtrasGalleryDisplay, AdvanceSlideshow, LOOP | UpdateExtrasGalleryDisplay | fade phase (256 → -64) |
| (var) | g_extrasGalleryCurrentSlidePtr | Update/Advance | Update (CrossFade src) | current slide surface |
| (consts) | g_extrasGallerySlideX/Y (`0x76`/`0x8c`=118/140) | Update/Advance | Update, CrossFade | slide draw pos |
| (var) | g_extrasGalleryPreviousSlideIndex_PROVISIONAL | Update / LoadBand(=-1) | Update | prev band track |
| (var) | g_extrasGalleryEnabledFlag_PROVISIONAL | InitializeFrontendPresentationState (CPUID bit23) | Update, LoadGallery | gallery enable / MMX-blend gate |
| (var) | g_frontendScreenTransitionFlag | SetFrontendScreen, Load*Gallery (0/1/2) | UpdateExtrasGalleryDisplay, fades | 2=band gallery active |
| (var) | g_attractCdTrackCandidate | extras/attract | Update (`+0x465e4c` band LUT index) | which CD track→band slide |
| `0x00465e4c` | band→slide LUT (12 B `01 03 04 04 02 00 00 01 03 04 04 04`) | static | UpdateExtrasGalleryDisplay | track index → slide 0..4 |
| `0x0049626c` | g_bodyTextFontSurface | font init | Draw*FontStringPrimary/Secondary/ToSurface | 12×12 body atlas (21 col) |
| `0x0049627c` | g_smallFontSurface | font init | DrawFrontendSmallFontStringToSurface, Clipped | 12×12 small atlas (21 col) |
| `0x00496278` | g_menuFontSurface | font init | MeasureOrDrawFrontendFontString, positioner | 36×36 large/menu atlas (7 col) |
| `0x00496268` | DAT_00496268 (button-frame page) | asset init | DrawFrontendButtonBackground | 9-slice button-frame source |
| `0x00496284` | g_browserSelectionBarSprite_PROVISIONAL | asset init | InitializeFrontendDisplayModeArrows | ◄► arrow source sprite |
| `0x00496288` | g_browserScrollIndicatorSprite_PROVISIONAL | asset init | DrawFrontendClippedStringToSurface | scroll-indicator glyph for ctrl chars |
| `0x00494bc0` | g_frontendDitherRampLut (1024 B) | BuildFrontendDitherOffsetTable | both fade fns | per-channel dither-step ramp |
| (var) | g_frontendFadeScanCursor / g_frontendFadeActive / _g_frontendFadeChannelR(GB) | Init/Reset/Render fade | fade fns, LOOP | fade band position/color |
| (var) | g_frontendSurfaceBitDepth (0xf or 0x10) | INIT | every pixel path | 555 vs 565 |
| (var) | g_frontendCanvasW / g_frontendCanvasH | INIT | clip, present, fills | virtual canvas size |
| (var) | g_frontendHardwareFlipEnabled / _Alt_PROVISIONAL | INIT/config | LOOP present branch / Copy16BitSurfaceRect fast path | HW vs SW present; Alt toggles direct-BltFast vs CPU-copy |
| `0x0049b680` | DAT_0049b680 (sprite-blit count) | QueueFrontendSpriteBlit (`++`, cap 0x40) | flush (reset 0) | per-frame sprite count |
| `0x0049b694` | DAT_0049b694 (preview-bake flag) | CreateFrontendDisplayModePreviewButton | CreateFrontendDisplayModeButton | bakes preview (half-bright) variant |

---

# PART 6 — ELEMENT-SOURCE MAP (synthesis)

For each visual element class: producing function(s), global(s)/asset(s), and whether drawn INLINE
by a screen-fn or by the deferred per-frame FLUSH.

| Visual element | Produced by | Reads / asset | INLINE vs FLUSH |
|---|---|---|---|
| **Menu button (frame+surface)** | CreateFrontendDisplayModeButton 0x425de0 (→ DrawFrontendButtonBackground 0x425b60, 9-slice from `DAT_00496268`) | writes `g_connBrowserListOriginX_PROVISIONAL[]` slot + new tracked surface | **INLINE** (screen builds the slot); the surface is later **drawn by FLUSH** via RenderFrontendUiRects→QueueFrontendSpriteBlit→flush sprite-loop |
| **Button label text** | baked INTO the button surface by CreateFrontendDisplayModeButton / RebuildFrontendButtonSurface via DrawFrontendLocalizedStringToSurface (24×24) or body 12×12 | font atlas `g_bodyTextFontSurface` | **INLINE bake** at button create; pixels reach screen via FLUSH (it’s part of the button surface) |
| **◄► cycle arrows** | InitializeFrontendDisplayModeArrows 0x426260 (baked into button surface, sets `flags|=2`) | `g_browserSelectionBarSprite_PROVISIONAL` | **INLINE bake**; on screen via FLUSH |
| **Selection / hover highlight** | RenderFrontendDisplayModeHighlight 0x4263e0 (4 edge bars color `0xc000`) | `g_frontendOverlayRectArrayTail_PROVISIONAL`, slot rect, `g_frontendBackSurfacePtr` | **FLUSH (decoupled)** |
| **Selected-button vertical offset** | RenderFrontendUiRects 0x425a30 (offsets the `g_frontendButtonIndex` slot src-rect by its bake height) | slot table | LOOP-queued → **FLUSH** |
| **Overlay rect / queued sprite** | QueueFrontendOverlayRect 0x425660 (→ `DAT_00498f40` static array) and QueueFrontendSpriteBlit 0x425730 (→ `DAT_00497ad0` cursor list) | per-frame arrays | enqueued INLINE/LOOP, **drawn by FLUSH** (overlay-drain plain-copy; sprite-loop color-keyed) |
| **Album / band cover art** | UpdateExtrasGalleryDisplay 0x40d830 → CrossFade16BitSurfaces 0x40d190 / AdvanceExtrasGallerySlideshow 0x40d750 | `g_extrasGallerySlideSurfaces`, phase, `g_attractCdTrackCandidate`, LUT@0x465e4c | **FLUSH (decoupled)** |
| **Screen fade in/out** | RenderFrontendFadeEffect 0x411780 / RenderFrontendFadeOutEffect 0x411a70 (64-row dither band) | `g_frontendDitherRampLut`, fade cursor/color | **INLINE** (screen-fn calls per frame; e.g. SCR2/4) |
| **Background fill / solid panel** | ClearBackbufferWithColor / FillPrimaryFrontendRect / FillSurfaceRectWithColor / BltColorFillToSurface / FillPrimaryFrontendScanline | 16bpp packed color → target surface | **INLINE** (screen draws to work/secondary surface) |
| **Full-screen background image** | LoadTgaToFrontendSurface16bpp 0x4125b0 (→ `g_primaryWorkSurface`) / 16bppVariant (→ secondary) | Extras/FrontEnd zips | **INLINE** (decode straight into work surface during screen setup) |
| **Text string (general)** | DrawFrontendFontString* / Localized* / SmallFont* / Clipped* / Wrapped* | atlases: body 12×12 (`0x49626c`), small 12×12 (`0x49627c`), menu 36×36 (`0x496278`), localized 24×24 | **INLINE** (screen-fn or baked into a label surface) |
| **Mouse cursor sprite** | LOOP queues a cursor overlay rect (`g_frontendCursorTextureId`) | cursor texture | LOOP-queued → **FLUSH** |
| **Positioner debug glyph ruler** | RenderPositionerGlyphStrip 0x414f40 (QueueFrontendOverlayRect 36×36 menu glyphs) | `g_menuFontSurface` | LOOP-queued (SCR1) → **FLUSH** |

**Key insight (the prior-pass miss):** a screen-fn makes most elements appear by **building slot
table entries / baking surfaces / enqueuing rects** (INLINE), but the actual on-screen pixels for
buttons, queued sprites, the gallery cover art, and the selection highlight are emitted by the
**per-frame FLUSH** (`FlushFrontendSpriteBlits` + its unconditional gallery/highlight calls + the
sprite-loop), reading the globals the screen primed. Only direct fills, fades, and full-screen
background TGAs are drawn inline into `g_primaryWorkSurface`/`g_secondaryWorkSurface`.

## [UNCERTAIN]
- `g_extrasGalleryCrossFadePhase`, `g_extrasGalleryCurrentSlidePtr`, the fade cursor/channel and
  selection-index globals were read by name in decompilation but their exact `.data` addresses were
  not individually resolved here (listed `(var)`); they live in the `0x0048f2xx`/`0x004951xx`/
  `0x004962xx` frontend-state region. Evidence missing: per-symbol `symbol_by_name` lookups (not run
  for these — they are unambiguous by usage but addresses are not asserted).
- `DAT_00496268` is the button-frame source page by usage in DrawFrontendButtonBackground; its
  exact load-site/asset filename was not traced (outside §3/4/5 decompile scope).
