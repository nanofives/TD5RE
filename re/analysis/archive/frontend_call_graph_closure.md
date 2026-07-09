# TD5 Frontend Subsystem — Provably-Complete Call-Graph Closure

RE target: `TD5_d3d.exe` (image base 0x00400000). Method: multi-source BFS over the
static call graph from ALL frontend roots, applying a BOUNDARY rule (stop at
DDraw/DXSound/DXInput/DXPlay middleware, CRT, pure math, and the archive/inflate
asset engine — record but do not descend). Pointer-dispatched / jump-table edges were
recovered separately (the direct-edge BFS misses them — this is the same class of bug
that made the prior pass miss the flush-driven gallery draw).

**BFS STATUS: EXHAUSTED.** No open frontier. Every queued node was expanded;
the pointer/jump-table frontier was expanded and closed (added only 1 leaf).

## Totals

| Category | Count |
|---|---|
| Frontend logic functions (closure) | **171** |
| — reached via direct call edges | 148 |
| — reached ONLY via function-pointer / jump table (recovered separately) | 22 |
| — pulled in by expanding that frontier | 1 |
| Archive/asset-engine boundary (recorded, not descended) | 8 |
| CRT / math boundary (internal leaves, recorded, not descended) | 137 |
| External boundary (DDraw/DXSound/DXInput/DXPlay/Win32 imports) | 77 |
| **DECOUPLED draws (reached via per-frame flush, NOT any screen fn)** | **5** (see below) |

The two `_PROVISIONAL`/`PTR_`-dispatched groups the prior pass missed: the
**Extras-Gallery** flush draws and the **controller-binding page** pointer table.

---

## 1. The 30 screen-fn table  (`g_frontendScreenFnTable` @ 0x004655C4, 30 × LE u32)

| idx | addr | name |
|---|---|---|
| 0 | 0x004269d0 | ScreenLocalizationInit |
| 1 | 0x00415030 | ScreenPositionerDebugTool |
| 2 | 0x004275a0 | RunAttractModeDemoScreen |
| 3 | 0x00427290 | ScreenLanguageSelect |
| 4 | 0x004274a0 | ScreenLegalCopyright |
| 5 | 0x00415490 | ScreenMainMenuAnd1PRaceFlow |
| 6 | 0x004168b0 | RaceTypeCategoryMenuStateMachine |
| 7 | 0x004213d0 | ScreenQuickRaceMenu |
| 8 | 0x00418d50 | RunFrontendConnectionBrowser |
| 9 | 0x00419cf0 | RunFrontendSessionPicker |
| 10 | 0x0041a7b0 | RunFrontendCreateSessionFlow |
| 11 | 0x0041c330 | RunFrontendNetworkLobby |
| 12 | 0x0041d890 | ScreenOptionsHub |
| 13 | 0x0041f990 | ScreenGameOptions |
| 14 | 0x0041df20 | ScreenControlOptions |
| 15 | 0x0041ea90 | ScreenSoundOptions |
| 16 | 0x00420400 | ScreenDisplayOptions |
| 17 | 0x00420c70 | ScreenTwoPlayerOptions |
| 18 | 0x0040fe00 | ScreenControllerBindingPage |
| 19 | 0x00418460 | ScreenMusicTestExtras |
| 20 | 0x0040dfc0 | CarSelectionScreenStateMachine |
| 21 | 0x00427630 | TrackSelectionScreenStateMachine |
| 22 | 0x00417d50 | ScreenExtrasGallery |
| 23 | 0x00413580 | ScreenPostRaceHighScoreTable |
| 24 | 0x00422480 | RunRaceResultsScreen |
| 25 | 0x00413bc0 | ScreenPostRaceNameEntry |
| 26 | 0x004237f0 | ScreenCupFailedDialog |
| 27 | 0x00423a80 | ScreenCupWonDialog |
| 28 | 0x00415370 | ScreenStartupInit |
| 29 | 0x0041d630 | ScreenSessionLockedDialog |

Root labels used in "reached-from" column: **LOOP**=RunFrontendDisplayLoop 0x414b50,
**FLUSH**=FlushFrontendSpriteBlits 0x425540, **INIT**=InitializeFrontendResourcesAndState
0x414740, **FSM**=SetFrontendScreen 0x414610 / InitializeFrontendDisplayModeState 0x414a90,
**SEED**=known draw/loader helper roots, **SCRn**=screen-fn index n, **PTR**=function-pointer/jump-table.

---

## 2. FSM / main loop

| addr | name | role (mechanical) | reached-from |
|---|---|---|---|
| 0x00414b50 | RunFrontendDisplayLoop | per-frame: polls input (DXInputGetKBStick/GetMouse), computes edge bits (g_frontendInputEdgeBits), UpdateFrontendClientOrigin, calls `(*g_currentScreenFnPtr)()`, queues cursor overlay rect, RenderFrontendUiRects → FlushFrontendSpriteBlits → present (DXDraw::Flip or PresentFrontendBufferSoftware), UpdateFrontendDisplayModeSelection, ESC→button, OptionsHub cheat-code key sequences (g_cheatCode*), increments g_frontendAnimFrameCounter, idle-50s→attract mode | LOOP |
| 0x00414610 | SetFrontendScreen | sets g_currentScreenFnPtr to a screen-table entry; releases tracked surfaces; resets selection/overlay state for the new screen | FSM, most-SCR |
| 0x00414a90 | InitializeFrontendDisplayModeState | builds display-mode list, writes Config.td5, releases tracked surfaces (screen-enter/leave bookkeeping) | FSM, many SCR |
| 0x00414740 | InitializeFrontendResourcesAndState | one-time resource/asset init: LoadFrontendSoundEffects, LoadExtrasGalleryImageSurfaces, BuildFrontendDitherOffsetTable, ClearFrontendSurfaceRegistry, InitializeFrontendPresentationState, reset overlay/selection/inline-string state, ActivateFrontendCursorOverlay, SerializeRaceStatusSnapshot | INIT |
| 0x004259b0 | AdvanceFrontendTickAndCheckReady | advances frontend animation tick, returns ready flag (screen step gate) | many SCR |
| 0x00418450 | NoOpHookStub | empty stub (per-frame hook point) | LOOP, INIT, SCR8-11,25 |
| 0x00425170 | UpdateFrontendClientOrigin | reads window client origin into frontend coords | LOOP |

---

## 3. Per-frame FLUSH / compositor + DECOUPLED draws  ⚠ (the prior-pass blind spot)

`FlushFrontendSpriteBlits` is the per-frame draw pass. It is called by the LOOP after the
screen fn, and it UNCONDITIONALLY calls two draws that NO screen fn ever calls — they read
state that screen fns primed. These are the DECOUPLED draws:

| addr | name | role (mechanical) | reached-from |
|---|---|---|---|
| 0x00425540 | FlushFrontendSpriteBlits | consumes the overlay-rect double-buffer at (&g_frontendOverlayRectArrayCount + g_frontendFrameToggle*0x410), blits up to 0x40 queued rects via Copy16BitSurfaceRect into g_primaryWorkSurface; then unconditionally calls UpdateExtrasGalleryDisplay + RenderFrontendDisplayModeHighlight; then blits the cursor-tracked sprite list (DAT_00497ad4, stride 0xc) via the surface vtable +0x74 + Copy16BitSurfaceRect; resets cursors | LOOP, FLUSH |
| 0x0040d830 | **UpdateExtrasGalleryDisplay** (DECOUPLED) | reads g_extrasGallerySlideSurfaces, g_extrasGalleryCrossFadePhase, g_frontendScreenTransitionFlag, g_attractCdTrackCandidate; cross-fades the band-gallery slide (CrossFade16BitSurfaces / Copy16BitSurfaceRect) at (0x76,0x8c); at phase -0x18 calls AdvanceExtrasGallerySlideshow | FLUSH (and LOOP via flush; SEED) |
| 0x004263e0 | **RenderFrontendDisplayModeHighlight** (DECOUPLED) | if g_frontendOverlayRectArrayTail != -1 and cursor not hidden, draws 4 edge bars (color 0xc000) around the selected menu rect in g_connBrowserListOriginX[] via BltColorFillToSurface(...,g_frontendBackSurfacePtr) | FLUSH (via flush) |
| 0x0040d750 | **AdvanceExtrasGallerySlideshow** (DECOUPLED) | advances gallery slide index / picks next slide surface; called from UpdateExtrasGalleryDisplay at fade end | FLUSH (via gallery) |
| 0x0040d190 | CrossFade16BitSurfaces (gallery helper) | alpha cross-fade blend of a 16bpp slide into work surface; called by UpdateExtrasGalleryDisplay | FLUSH, SEED |
| 0x00425730 | QueueFrontendSpriteBlit | appends a sprite blit record (origin/src/size/color/surface) to the per-frame sprite list | LOOP (via RenderFrontendUiRects) |
| 0x00425a30 | RenderFrontendUiRects | walks 3 button/rect arrays (DAT_0049a9a4 preview, g_connBrowserListOriginX menu buttons, DAT_00498f64) and emits QueueFrontendSpriteBlit for each visible entry; offsets the selected entry (g_frontendButtonIndex) | LOOP |
| 0x00426580 | UpdateFrontendDisplayModeSelection | post-present: advances/animates the highlighted display-mode/button selection state | LOOP |
| 0x00425660 | QueueFrontendOverlayRect | appends an overlay rect (x,y,src,w,h,color,texid) to the overlay double-buffer | LOOP, many SCR, SEED |
| 0x00425360 | PresentFrontendBufferSoftware | software path: copies work surface to the visible surface | LOOP |
| 0x004251a0 | Copy16BitSurfaceRect | core 16bpp rect blit with color-key/mode flag | LOOP, FLUSH, INIT, many SCR, SEED |
| 0x00424b80 | BlitFrontendCachedRect | restores a cached background rect to the work surface (redraw) | LOOP |

---

## 4. Draw / text / fill helpers

| addr | name | role | reached-from |
|---|---|---|---|
| 0x00423db0 | ClearBackbufferWithColor | fills backbuffer with a solid color | SCR1,20 |
| 0x00423ed0 | FillPrimaryFrontendRect | fills a rect on the primary frontend surface | SCR20, INIT |
| 0x00423f90 | FillSurfaceRectWithColor | fills an arbitrary surface rect | SCR22 |
| 0x00424050 | BltColorFillToSurface | color-fill blit to a given surface (used for highlight bars, panels) | most-SCR, LOOP, FLUSH |
| 0x00423e40 | LockSecondaryFrontendSurfaceFillColor | locks secondary surface + fills with color (leaf) | PTR (caseD_6) |
| 0x00424110 | DrawFrontendFontStringPrimary | draws a font string to the primary surface | SCR3,20 |
| 0x004241e0 | DrawFrontendFontStringSecondary | draws a font string to the secondary surface | SCR4 |
| 0x004242b0 | DrawFrontendLocalizedStringPrimary | draws a LANGUAGE.DLL localized string to primary | SCR3,20 |
| 0x00424390 | DrawFrontendLocalizedStringSecondary | localized string to secondary | SCR4 |
| 0x00424470 | DrawFrontendFontStringToSurface | draws font string to a target surface | most-SCR, SEED |
| 0x00424560 | DrawFrontendLocalizedStringToSurface | draws localized string to a target surface | most-SCR, SEED |
| 0x00424660 | DrawFrontendSmallFontStringToSurface | small-font string to surface | SCR6,11,20,23,25 |
| 0x00424740 | DrawFrontendClippedStringToSurface | clipped string to surface (lists/chat) | SCR8,9,10,11,23,25 |
| 0x00424890 | MeasureOrCenterFrontendString | measures/centers a font string | SCR6,20,23,25 |
| 0x004248e0 | DrawFrontendWrappedStringLine | word-wrap line draw | SCR11 |
| 0x00424a50 | MeasureOrCenterFrontendLocalizedString | measures/centers a localized string | most-SCR, SEED |
| 0x00412d50 | MeasureOrDrawFrontendFontString | measure-or-draw font string (dual-mode) | most-SCR |
| 0x00412e30 | CreateMenuStringLabelSurface | bakes a text label into a new tracked surface | most-SCR |
| 0x00424d90 | FillPrimaryFrontendScanline | fills a scanline on primary | SCR1 |
| 0x00411750 | InitFrontendFadeColor | sets the fade color | SCR2,4 |
| 0x00411780 | RenderFrontendFadeEffect | renders a fade overlay (fade-in) | SCR2,4 |
| 0x00411a50 | ResetFrontendFadeState | resets fade state | SCR4 |
| 0x00411a70 | RenderFrontendFadeOutEffect | renders a fade-out overlay | SCR4 |
| 0x00411710 | BuildFrontendDitherOffsetTable | builds the dither-offset table used by fades/blits | INIT |
| 0x00425b60 | DrawFrontendButtonBackground | draws a button background panel | most-SCR, SEED |
| 0x00425de0 | CreateFrontendDisplayModeButton | creates a display-mode/menu button slot + surface | most-SCR, SEED |
| 0x004260e0 | CreateFrontendDisplayModePreviewButton | creates a preview-image button slot | SCR6,16,20,24 |
| 0x00426120 | RebuildFrontendButtonSurface | re-bakes a button surface (label/state change) | SCR20,21,23,24,25 |
| 0x00426260 | InitializeFrontendDisplayModeArrows | creates the ◄► cycle-arrow button slots | SCR7,13-17,19-24, SEED |
| 0x00426390 | ReleaseFrontendDisplayModeButtons | frees button slots/surfaces | most-SCR |
| 0x004264e0 | BeginFrontendDisplayModePreviewLayout | begins preview layout (stores prior layout ptr) | SCR5,11 |
| 0x00426540 | RestoreFrontendDisplayModePreviewLayout | restores prior preview layout | SCR5,11 |
| 0x004258f0 | CreateFrontendMenuRectEntry | adds a menu rect entry (hit/draw region) | SCR3, SEED |
| 0x004259d0 | MoveFrontendSpriteRect | repositions a sprite rect | many SCR, SEED |
| 0x00424f40* | RenderPositionerGlyphStrip (0x00414f40) | debug positioner: draws a glyph strip | SCR1 |

(*) line corrected: RenderPositionerGlyphStrip = 0x00414f40.

### Present / copy helpers
| addr | name | role | reached-from |
|---|---|---|---|
| 0x00424af0 | PresentPrimaryFrontendBufferViaCopy | present primary via surface copy | many SCR |
| 0x00424b30 | CopyPrimaryFrontendBufferToSecondary | copy primary→secondary | many SCR |
| 0x00424bc0 | CopyPrimaryFrontendRectToSecondary | copy a primary rect→secondary | SCR20 |
| 0x00424c10 | PresentSecondaryFrontendRectViaCopy | present a secondary rect via copy | many SCR |
| 0x00424c50 | BlitSecondaryFrontendRectToPrimary | blit secondary rect→primary | many SCR |
| 0x00424ca0 | PresentPrimaryFrontendBuffer | present primary buffer | SCR2,3,20 |
| 0x00424cf0 | PresentSecondaryFrontendRect | present secondary rect | SCR20 |
| 0x00424d40 | PresentPrimaryFrontendRect | present primary rect | SCR20 |
| 0x00424e40 | InitializeFrontendPresentationState | init present surfaces/state | INIT |
| 0x004258b0 | DeferFrontendBackgroundRestore | marks a bg rect for deferred restore | SCR20 |

---

## 5. Asset / surface loaders (frontend)

| addr | name | role | reached-from |
|---|---|---|---|
| 0x00412030 | LoadFrontendTgaSurfaceFromArchive | loads a TGA from the archive into a tracked surface | most-SCR, INIT |
| 0x004122f0 | LoadTgaToFrontendSurfaceFromArchive | loads TGA from archive to a frontend surface | SCR19, SEED, PTR(gallery) |
| 0x004125b0 | LoadTgaToFrontendSurface16bpp | decodes TGA → 16bpp surface | many SCR |
| 0x004127b0 | LoadTgaToFrontendSurface16bppVariant | 16bpp TGA decode (variant) | SCR4,8 |
| 0x004129b0 | RenderTgaToFrontendSurface | renders/blits a TGA onto a surface | many SCR, PTR(caseD_a) |
| 0x00412b00 | SetSurfaceColorKeyFromRGB | sets surface color key from an RGB value | many SCR, PTR(caseD_a) |
| 0x00412b90 | RenderTgaWithColorKeyToSurface | renders TGA honoring color key | SCR20 |
| 0x00411f00 | CreateTrackedFrontendSurface | allocates + registers a tracked surface | most-SCR, INIT, SEED |
| 0x00411de0 | ClearFrontendSurfaceRegistry | clears the tracked-surface registry | INIT |
| 0x00411e00 | GetFrontendSurfaceRegistryId | looks up a registry id for a surface | most-SCR, LOOP, SEED |
| 0x00411e30 | ReleaseTrackedFrontendSurface | releases one tracked surface | most-SCR, SEED |
| 0x00411e90 | ReleaseTrackedFrontendSurfaces | releases all tracked surfaces | many SCR, FSM |
| 0x00414640 | LoadFrontendSoundEffects | loads frontend SFX buffers (DXSound LoadBuffer) | INIT |
| 0x0040d590 | LoadExtrasGalleryImageSurfaces | loads the extras-gallery image surfaces | SCR19, INIT |
| 0x0040d640 | ReleaseExtrasGalleryImageSurfaces | frees extras-gallery image surfaces | SCR19 |
| 0x0040d6a0 | LoadExtrasBandGalleryImages | loads the band-gallery slide images | SCR19, SEED |
| 0x00417dd2 | LoadFrontendExtrasGalleryResources | loads gallery resources (TGAs) — POINTER-reached, no direct caller | PTR |

---

## 6. Screen functions (closure leaves of each screen) + screen-local helpers

| addr | name | role | reached-from |
|---|---|---|---|
| 0x004269d0 | ScreenLocalizationInit | idx0: loads packed config, formats display-mode option strings (language init) | SCR0 |
| 0x0040fb60 | LoadPackedConfigTd5 | loads Config.td5 packed blob | SCR0 |
| 0x0041d840 | FormatDisplayModeOptionStrings | formats resolution/mode option strings | SCR0 |
| 0x00415030 | ScreenPositionerDebugTool | idx1: debug element-positioner tool screen | SCR1 |
| 0x00414f40 | RenderPositionerGlyphStrip | draws positioner glyph strip | SCR1 |
| 0x004275a0 | RunAttractModeDemoScreen | idx2: attract-mode demo screen | SCR2 |
| 0x00427290 | ScreenLanguageSelect | idx3: language-select screen | SCR3 |
| 0x004274a0 | ScreenLegalCopyright | idx4: legal/copyright screen w/ fade | SCR4 |
| 0x00415490 | ScreenMainMenuAnd1PRaceFlow | idx5: main menu + 1P race flow state machine | SCR5 |
| 0x004168b0 | RaceTypeCategoryMenuStateMachine | idx6: race-type/category menu FSM | SCR6 |
| 0x00417b74 | AdvanceFrontendInlineStringTableState | advances inline-string table animation | SCR6 |
| 0x004213d0 | ScreenQuickRaceMenu | idx7: quick-race menu | SCR7 |
| 0x00418d50 | RunFrontendConnectionBrowser | idx8: net connection browser | SCR8 |
| 0x0040cdc0 | CrossFade16BitSurfaces (SCR8 variant) | cross-fade helper used by connection browser | SCR8 |
| 0x0040d120 | AdvanceCrossFadeTransition | advances a cross-fade transition | SCR8 |
| 0x00419cf0 | RunFrontendSessionPicker | idx9: net session picker | SCR9 |
| 0x00419b30 | RenderFrontendSessionBrowser | renders the session list | SCR9 |
| 0x0041a7b0 | RunFrontendCreateSessionFlow | idx10: create-session flow | SCR10 |
| 0x0041a530 | RenderFrontendCreateSessionNameInput | renders session-name text input | SCR10,25 |
| 0x0041c330 | RunFrontendNetworkLobby | idx11: network lobby | SCR11 |
| 0x0041a670 | RenderFrontendLobbyChatInput | renders lobby chat input | SCR11 |
| 0x0041b420 | RenderFrontendLobbyStatusPanel | renders lobby status panel | SCR11 |
| 0x0041bd00 | RenderFrontendLobbyChatPanel | renders lobby chat panel | SCR11 |
| 0x0041c030 | NormalizeFrontendChatTokens | normalizes chat tokens | SCR11 |
| 0x0041b390 | CreateFrontendNetworkSession | creates a net session | SCR20,21 |
| 0x0041b610 | ProcessFrontendNetworkMessages | processes inbound net messages | SCR11,20 |
| 0x00418c60 | QueueFrontendNetworkMessage | queues an outbound net message | SCR10,11,20,21 |
| 0x0041d890 | ScreenOptionsHub | idx12: options hub | SCR12 |
| 0x0041f990 | ScreenGameOptions | idx13: game options | SCR13 |
| 0x0041df20 | ScreenControlOptions | idx14: control options | SCR14 |
| 0x0041ea90 | ScreenSoundOptions | idx15: sound options | SCR15 |
| 0x00420400 | ScreenDisplayOptions | idx16: display options | SCR16 |
| 0x00420c70 | ScreenTwoPlayerOptions | idx17: 2-player options | SCR17 |
| 0x0040fe00 | ScreenControllerBindingPage | idx18: controller key-binding page (dispatches binding-page handlers via PTR table) | SCR18 |
| 0x004100ce | DrawControlBindingTextWithOkButton | draws binding text + OK button | SCR18 |
| 0x00418460 | ScreenMusicTestExtras | idx19: music-test / extras screen (switch via jump table → caseD_6/7/a) | SCR19 |
| 0x0040dfc0 | CarSelectionScreenStateMachine | idx20: car-select FSM | SCR20 |
| 0x0040ddc0 | DrawCarSelectionPreviewOverlay | draws car preview overlay | SCR20 |
| 0x00427630 | TrackSelectionScreenStateMachine | idx21: track-select FSM | SCR21 |
| 0x00417d50 | ScreenExtrasGallery | idx22: extras gallery screen | SCR22 |
| 0x00413580 | ScreenPostRaceHighScoreTable | idx23: post-race high-score table | SCR23 |
| 0x00413010 | DrawPostRaceHighScoreEntry | draws one high-score row | SCR23,25 |
| 0x00422480 | RunRaceResultsScreen | idx24: race results screen | SCR24 |
| 0x00421e90 | DrawRaceDataSummaryPanel | draws race-data summary panel | SCR24 |
| 0x0040a880 | ResetRaceResultsTable | clears the results table | SCR6,24 |
| 0x0040aad0 | SortRaceResultsByPrimaryMetricAsc | sort results ascending (primary) | SCR24 |
| 0x0040ab80 | SortRaceResultsBySecondaryMetricDesc | sort results descending (secondary) | SCR24 |
| 0x0040df80 | MarkMastersRaceSlotCompleted | marks a masters-cup race slot done | SCR24 |
| 0x00421da0 | AwardCupCompletionUnlocks | awards cup-completion unlocks | SCR24 |
| 0x00413bc0 | ScreenPostRaceNameEntry | idx25: name-entry screen | SCR25 |
| 0x004237f0 | ScreenCupFailedDialog | idx26: cup-failed dialog | SCR26 |
| 0x00423a80 | ScreenCupWonDialog | idx27: cup-won dialog | SCR27 |
| 0x00415370 | ScreenStartupInit | idx28: startup init screen | SCR28 |
| 0x0041d630 | ScreenSessionLockedDialog | idx29: session-locked dialog | SCR29 |
| 0x00410ca0 | ConfigureGameTypeFlags | configures game-type flags (cup/race) | SCR6,24 |
| 0x0040dac0 | InitializeRaceSeriesSchedule | builds the race-series schedule | many SCR |
| 0x0040b100 | BuildEnumeratedDisplayModeList | enumerates available display modes | many SCR, FSM |

---

## 7. Inline-string table / state utils

| addr | name | role | reached-from |
|---|---|---|---|
| 0x004183b0 | SetFrontendInlineStringTable | sets the inline-string table | many SCR |
| 0x00418410 | SetFrontendInlineStringEntry | sets one inline-string entry | SCR5,14,20,21 |
| 0x00418430 | ResetFrontendInlineStringTable | resets inline-string table | many SCR, INIT |
| 0x004254d0 | ResetFrontendOverlayState | resets overlay double-buffer state | INIT |
| 0x00425500 | ResetFrontendSelectionState | resets selection/highlight state | INIT |
| 0x004258c0 | ActivateFrontendCursorOverlay | shows the cursor overlay | most-SCR, INIT |
| 0x004258e0 | DeactivateFrontendCursorOverlay | hides the cursor overlay | many SCR |

---

## 8. Save / cup-data / race-snapshot (frontend-invoked persistence)

| addr | name | role | reached-from |
|---|---|---|---|
| 0x0040f8d0 | WritePackedConfigTd5 | writes Config.td5 packed blob | many SCR, FSM |
| 0x00411120 | SerializeRaceStatusSnapshot | serializes race-status snapshot | INIT |
| 0x004112c0 | RestoreRaceStatusSnapshot | restores race-status snapshot | SCR6,24 |
| 0x004114f0 | WriteCupData | writes CupData.td5 | SCR24 |
| 0x00411590 | LoadContinueCupData | loads continue-cup data | SCR6 |
| 0x00411630 | ValidateCupDataChecksum | validates cup-data checksum | SCR6 |

---

## 9. Controller-binding page handlers — POINTER-DISPATCHED (frontier, recovered)

Reached only via the THUNK/COMPUTED_JUMP at 0x004100d7 + pointer table at 0x00410c84,
from ScreenControllerBindingPage (idx18). The direct-edge BFS MISSED these.

| addr | name | role | reached-from |
|---|---|---|---|
| 0x004100c0 | OpenControllerBindingPageWrapper | binding-page open wrapper → DrawFrontendLocalizedStringToSurface | PTR |
| 0x004100de | OpenControllerBindingPageRearViewHeader | rear-view header page (leaf) | PTR |
| 0x004100fa | DrawControlBindingText1WithOkButton | binding text variant 1 + OK | PTR |
| 0x00410111 | DrawControlBindingText2WithOkButton | binding text variant 2 + OK | PTR |
| 0x00410129 | OpenControllerBindingPageNoneHeader | "none" header page → button + localized string | PTR |
| 0x00410380 | RenderControllerBindingMenuPage | renders the binding menu page | PTR |
| 0x0041043c | RenderControllerBindingPageUpDownHeader | up/down header | PTR |
| 0x004104b2 | RenderControllerBindingPageDownHeader | down header | PTR |
| 0x00410527 | RenderControllerBindingPageUpHeader | up header | PTR |
| 0x00410599 | RenderControllerBindingPageBlankOrRearViewHeader | blank/rear-view header | PTR |
| 0x00410613 | RenderControllerBindingPageRows | renders binding rows | PTR |
| 0x00410940 | DrawControlOptionsBindingHeader | draws binding header (centered) | PTR |

### Cup-schedule configurators — DATA-table dispatched (table @ 0x00464108)
| addr | name | role | reached-from |
|---|---|---|---|
| 0x00410f60 | ConfigureCupChampionshipSchedule | builds championship cup schedule (leaf) | PTR |
| 0x00410fa0 | ConfigureCupEraSchedule | builds era cup schedule | PTR |
| 0x00410ff0 | ConfigureCupChallengeSchedule | builds challenge cup schedule | PTR |
| 0x00411030 | ConfigureCupPitbullSchedule | builds pitbull cup schedule | PTR |
| 0x00411070 | ConfigureCupMastersSchedule | builds masters cup schedule | PTR |
| 0x004110a0 | ConfigureCupUltimateSchedule | builds ultimate cup schedule | PTR |

### Jump-table case fragments (idx19 ScreenMusicTestExtras switch, COMPUTED_JUMP)
| addr | name | role | reached-from |
|---|---|---|---|
| 0x004173b1 | caseD_7 | music-test switch case: move sprite + queue overlay | PTR |
| 0x00417700 | caseD_a | music-test switch case: load/render TGA + set screen | PTR |
| 0x0041808d | caseD_6 | music-test switch case: lock secondary surface + fill | PTR |
| 0x00423e40 | LockSecondaryFrontendSurfaceFillColor | locks secondary surface + fills (called by caseD_6, leaf) | PTR |

NOTE: caseD_6/7/a are Ghidra-named switch case blocks; they carry frontend-region
addresses and are reached through the idx19 jump table, but the `caseD_a` symbol is also
COMPUTED_JUMP-referenced from non-frontend code (0x402948 etc.) — i.e. it is a shared
case-block label. Treated here as frontend because of its idx19 reach and its callees.

---

## 10. Key frontend GLOBALS (the screen-fn ⇆ flush contract)

| addr/symbol | name | written-by | consumed-by |
|---|---|---|---|
| 0x004655C4 | g_frontendScreenFnTable (30 ptrs) | static data | LOOP (`*g_currentScreenFnPtr`), SetFrontendScreen |
| (var) | g_currentScreenFnPtr | SetFrontendScreen, LOOP attract-switch | LOOP dispatch |
| (var) | g_frontendFrameToggle | LOOP (^=1 each present) | FlushFrontendSpriteBlits (selects 0x410 overlay bank) |
| &g_frontendOverlayRectArrayCount_PROVISIONAL | overlay-rect double-buffer (2×0x410) | QueueFrontendOverlayRect | FlushFrontendSpriteBlits (Copy16BitSurfaceRect loop) |
| g_frontendOverlayRectCursor_PROVISIONAL | overlay defer counter | DeferFrontendBackgroundRestore | FlushFrontendSpriteBlits (skip-1 logic) |
| g_frontendOverlayRectArrayTail_PROVISIONAL | selected-rect index | selection update | RenderFrontendDisplayModeHighlight (which rect to outline) |
| g_frontendButtonIndex | highlighted button index | input/selection, ESC handler | RenderFrontendUiRects (offset selected), input |
| g_frontendEscKeyButtonIndex | ESC→button mapping | screen fns | LOOP (CheckKey(1)) |
| g_frontendButtonPressedFlag | button-press latch | LOOP ESC, input | screen fns |
| g_frontendCursorOverlayHidden | cursor/highlight visibility | Activate/DeactivateFrontendCursorOverlay | LOOP, RenderFrontendUiRects, RenderFrontendDisplayModeHighlight |
| g_frontendMouseCursorEnabled | mouse cursor enable | options | LOOP (cursor rect queue) |
| g_frontendCursorTextureId | cursor texture id | INIT | LOOP (QueueFrontendOverlayRect) |
| g_connBrowserListOriginX_PROVISIONAL[] | menu-button slot array (FrontendButtonSlot, stride 0xd dwords) | CreateFrontendDisplayModeButton/MenuRectEntry | RenderFrontendUiRects, RenderFrontendDisplayModeHighlight |
| DAT_0049a9a4 | preview-button slot array | CreateFrontendDisplayModePreviewButton | RenderFrontendUiRects |
| DAT_00498f64 | static rect/sprite slot array | menu setup | RenderFrontendUiRects |
| DAT_00497ad4 | cursor-tracked sprite list (stride 0xc) | sprite setup | FlushFrontendSpriteBlits (final blit loop) |
| g_frontendSurfaceRegistryTail / registry | tracked-surface registry | CreateTrackedFrontendSurface | Release*, FlushFrontendSpriteBlits (match loop) |
| g_primaryWorkSurface | primary work surface ptr | INIT | FlushFrontendSpriteBlits, gallery, blits |
| g_frontendBackSurfacePtr | back/secondary surface ptr | INIT | RenderFrontendDisplayModeHighlight, ScreenDump |
| g_extrasGallerySlideSurfaces (+ array) | gallery slide surface ptrs | LoadExtrasBandGalleryImages | UpdateExtrasGalleryDisplay |
| g_extrasGalleryCrossFadePhase | gallery fade phase counter | UpdateExtrasGalleryDisplay, LOOP attract-gate | UpdateExtrasGalleryDisplay, LOOP |
| g_extrasGalleryCurrentSlidePtr | current slide surface | UpdateExtrasGalleryDisplay | UpdateExtrasGalleryDisplay (CrossFade) |
| g_extrasGallerySlideX/Y (0x76/0x8c) | slide draw position | UpdateExtrasGalleryDisplay | CrossFade16BitSurfaces |
| g_extrasGalleryPreviousSlideIndex_PROVISIONAL | prev slide index | UpdateExtrasGalleryDisplay | UpdateExtrasGalleryDisplay |
| g_extrasGalleryEnabledFlag_PROVISIONAL | gallery enable flag | extras screen | UpdateExtrasGalleryDisplay |
| g_frontendScreenTransitionFlag | screen-transition state (0/1/2) | SetFrontendScreen, attract | UpdateExtrasGalleryDisplay, fades |
| g_attractCdTrackCandidate | attract/gallery slide selector | extras/attract | UpdateExtrasGalleryDisplay |
| g_attractModeTrackIndex | attract demo track index | LOOP (rand) | RunAttractModeDemoScreen |
| g_frontendInputCurrentBits / PreviousBits / EdgeBits | input state + edges | LOOP (DXInputGetKBStick) | all screen fns |
| g_frontendMouse* (PrevX/Y, EdgeBits, MovedFlag) | mouse state | LOOP (GetMouse) | screen fns, cursor |
| g_frontendAnimFrameCounter | per-frame animation counter | LOOP (++) | animations, attract idle |
| g_frontendFrameTimestamp_ms / g_frontendInnerState | timing for attract idle | LOOP (timeGetTime) | LOOP attract gate |
| g_frontendRedrawCount | full-redraw request count | LOOP (surface lost) | LOOP (BlitFrontendCachedRect) |
| g_startRaceRequestFlag / g_startRaceConfirmFlag | exit-loop-to-race flags | screen fns | LOOP (early return) |
| g_cheatCodeKeySequences / KeyProgress / TargetPointers / XorMaskTable | cheat-code FSM | static + LOOP | LOOP (OptionsHub cheat handler), gNpcRacerCheatFlags |
| gNpcRacerCheatFlags / g_npcRacerGroupTable | NPC-unlock cheat flags | LOOP cheat handler | car select |
| g_frontendHardwareFlipEnabled | HW vs SW present | INIT/config | LOOP present branch |

---

## 11. Boundary — archive / asset-engine (recorded, NOT descended)

Reached via LoadFrontendTgaSurfaceFromArchive / Load*FromArchive (asset path).
These are the shared decompression/archive engine, not frontend logic.

| addr | name |
|---|---|
| 0x0043fb70 | ReadTrackStaticDataChunk |
| 0x0043fb90 | ReadCompressedTrackStreamChunk |
| 0x0043fbc0 | DecompressTrackDataStream |
| 0x0043fc80 | ParseZipCentralDirectory |
| 0x004405b0 | DecompressZipEntry |
| 0x00440790 | ReadArchiveEntry |
| 0x00440860 | OpenArchiveFileForRead |
| 0x004409b0 | GetArchiveEntrySize |

(Their descendants — the InflateDecompress family 0x00447490–0x00447fe2 — were reached and
counted in the CRT/engine boundary; they are inflate primitives, not frontend logic.)

## 12. Boundary — CRT / math internal leaves (137; recorded, NOT descended)

All functions at addr ≥ 0x00447490 in the reached set: malloc/free/heap (`_malloc`,
`_heap_alloc`, `__sbh_*`), file IO (`_fopen`,`_fread`,`_fwrite`,`_fseek`,`_read`,`_write`,
`_sopen`,`_lseek`), string/locale (`_strcpy`,`_strlen`,`_strncmp`,`_strlwr`,`stricmp_game`,
`__crtLCMapStringA`,`__crtGetStringTypeA`,`_mbtowc`,`_wctomb`), printf/scanf family
(`sprintf_game`,`_sscanf`,`_output`,`_input`), 64-bit math (`__allmul`,`__alldiv`,
`__aulldiv`,`__aullrem`,`__allshl`), `_rand`, `_memcpy`/`_memset`/`_memmove`, startup/exit
(`__initterm`,`_cexit`,`__amsg_exit`). (`_rand`/`_memcpy` etc. are the canonical CRT leaves
the boundary rule names.) Full list is in `_fe_closure.json` (addr ≥ 0x447490).

## 13. Boundary — external imports (77; DDraw/DXSound/DXInput/DXPlay/Win32)

DXDraw/DDraw: Flip, ClearBuffers, CanDo3D, CanFog, ImageProTGA, GetVolume, SetVolume,
SetAnsi, GetString, SetPlayback, LoadBuffer, Allocate, DeAllocate, Destroy.
DXSound/CD: Play, Stop, CDPlay, CDSetVolume, CDGetVolume.
DXInput: DXInputGetKBStick, GetMouse, CheckKey, GetJS.
DXPlay / M2DX net: ConnectionEnumerate, ConnectionPick, JoinSession, NewSession,
SealSession, ReceiveMessage, SendMessageA, EnumSessionTimer.
Diagnostics: Msg, DXErrorToString, CleanUpAndPostQuit.
Win32: CreateFileA, ReadFile, WriteFile, SetFilePointer, SetEndOfFile, GetFileType,
CloseHandle, DeleteFileA, GetLastError/SetLastError, HeapAlloc/HeapFree/HeapReAlloc,
VirtualAlloc/VirtualFree, Get/SetStdHandle, LoadLibraryA, GetProcAddress,
GetModuleFileNameA, GetComputerNameA, ClientToScreen, SendMessageA, EnterCriticalSection,
LeaveCriticalSection, InitializeCriticalSection, Interlocked*, Tls*, QueryPerformance*,
timeGetTime, MultiByteToWideChar/WideCharToMultiByte, LCMapStringA/W, GetStringTypeA/W,
ExitProcess/TerminateProcess/GetCurrentProcess/Thread.

### [UNCERTAIN] boundary classifications
- `OpenControllerBindingPageNoneHeader` was REPORTED as external by the call-graph API but
  is actually an INTERNAL frontend function (0x00410129) invoked via a THUNK/pointer
  (0x004100d7 / 0x00410c84). Reclassified into §9 (frontend), NOT a real external.
  This is the indirect-edge that the direct BFS missed.
- `OpenControllerBindingPageRearViewHeader`, `caseD_6/7/a` similarly are internal jump/ptr
  targets, not imports.

---

## FRONTIER
NONE — BFS exhausted. The pointer/jump-table frontier (§9, 22 functions) was fully
expanded; it pulled in exactly one additional leaf (LockSecondaryFrontendSurfaceFillColor
0x00423e40), which calls nothing further. All other edges terminate at the boundary sets
(§11–13). Running total of in-closure frontend functions: **171**.

Raw data: `re/analysis/_fe_closure.json` (293 direct-edge internal nodes with per-node
callees + reached-from root labels; the 22 pointer-reached + 1 leaf are documented in §9).
