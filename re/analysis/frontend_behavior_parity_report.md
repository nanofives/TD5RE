# TD5 Frontend — Button & Element Behavior Parity Report

**Generated 2026-05-30** by a full Ghidra behavior+completeness audit of all 30 frontend screen functions, each diffed against the port (`td5_frontend.c`). Companion to `frontend_screen_layout_spec.md` (positions). This report covers **what each button/element DOES** and **whether the port implements it faithfully**.

## Architectural verdict (the good news)

**The port's button-dispatch architecture is FAITHFUL.** Every one of the 25 `CreateFrontendDisplayModeButton` call sites across all interactive screens passes `userdata = 0` (literal). The original dispatches a press by switching on the **button index** (`g_frontendButtonIndex`, = creation order) combined with `g_frontendInnerState` — there is **no userdata/callback model**. The port's index-switch on `s_selected_button` (with `FE_Button` carrying no action field) therefore mirrors the original exactly. Dropping the 6th `userdata` arg was correct.

Consequence: button-completeness comes down to **(a) same set, (b) same creation order, (c) same gating conditions** per screen — and behavior comes down to the per-index switch doing the same navigate/toggle/command. Most screens pass. The gaps below are specific, not architectural.

## Coverage summary (30 screens)

| Bucket | Screens |
|---|---|
| **Faithful** (button set + behavior + elements match) | 0, 2, 4, 5, 7, 21, 23, 24, 26, 27, 29 |
| **Partial** (works, but missing elements or a sub-behavior) | 8, 9*, 10, 11, 13, 15, 16, 18, 20, 25 |
| **Divergent** (port does something materially different) | 1, 3, 12, 14, 22, 28 |

\*Screen 9 also has a Wrong item (spurious Create button + lost session-join).

## Prioritized gap matrix

### P0 — Functional bugs (a button/option misbehaves or silently does nothing)

| # | Screen | Bug | Original behavior | Port behavior | Port ref |
|---|--------|-----|-------------------|---------------|----------|
| P0-1 | 12 OptionsHub | **OK doesn't commit options** | OK commits option shadows → live globals (`gRaceDifficultyTier`, dynamics, `3dCollisions`→`g_cameraMode^1`, special-encounter A/B) | OK only navigates to MAIN_MENU; shadows discarded | td5_frontend.c (OptionsHub OK case) [UNCERTAIN: may commit at race-start instead] |
| P0-2 | 14 ControlOptions | **Device assignment uneditable** | ◄► over Player1/Player2 rows cycles `g_player1/2InputSource` | No delta handler; rows can't change device | td5_frontend.c (ControlOptions) |
| P0-3 | 14 ControlOptions | Device-name panel not drawn | Renders `"%s"` (DAT_004658e4) / `"%s %d"` (0x46603c) device name + row arrows | Panel + arrows missing | — |
| P0-4 | 13 GameOptions | Circuit-Laps wrap vs clamp | Laps value CLAMPS [0,3] (:7582-83) | Port WRAPS | td5_frontend.c:~7582 |
| P0-5 | 15 SoundOptions | SFX mode 2-state vs 3-state | 3-row Controllers.tga (mono/stereo/surround) | 2-state Mono/Stereo (`&1`); 3rd mode shows Mono; mode text omitted | td5_frontend.c (SoundOptions) |
| P0-6 | 19 MusicTest | Track index clamp vs wrap | Track index WRAPS 0↔0xb | Port CLAMPS 0..11 (comment at :8448 wrongly claims orig clamps) | td5_frontend.c:~8448 |
| P0-7 | 16 DisplayOptions | Fog row always live | Fog row is a disabled preview button when `DXD3D::CanFog()!=1` | Always a live cycler | td5_frontend.c (DisplayOptions) |
| P0-8 | 9 SessionPicker | Spurious button + lost join | OK joins the selected enumerated session; 3 buttons | Port adds a 4th non-original "Create" button (index shift); OK no longer joins | td5_frontend.c (SessionPicker) [DXPTYPE-adjacent] |
| P0-9 | 25 NameEntry | Highlight wrong row | High-score highlight follows inserted rank (`s_score_insert_pos`) | Hard-coded to row 0 | td5_frontend.c:4716 |
| P0-10 | 28 StartupInit | Possible wrong redirect | case-4 hands off to `g_frontendScreenFnTable[0]` | Port hands off to TD5_SCREEN_LOCALIZATION_INIT | [UNCERTAIN: correct IF table[0]==LocalizationInit — VERIFY] |

### P1 — Completeness (missing buttons/elements; mostly netplay-stubbed or display-only)

| # | Screen | Missing | Notes |
|---|--------|---------|-------|
| P1-1 | 8 ConnectionBrowser | list rows, scroll arrows, selection bar | DXPTYPE (netplay) — stubbed by design |
| P1-2 | 9 SessionPicker | list rows, scroll arrows, selection bar | DXPTYPE |
| P1-3 | 10 CreateSessionFlow | player-name 2nd stage + empty→computer-name fallback | DXPTYPE-adjacent |
| P1-4 | 11 NetworkLobby | partial lobby UI | DXPTYPE |
| P1-5 | 18 ControllerBinding | live joystick (DXInput) + wheel path | KB capture is wired/faithful; joystick uses KB-arrow surrogate; wheel unported |
| P1-6 | 20 CarSelect | Info_Values stat sub-panel | no-op stub; Back-button gate differs (`!s_network_active` vs orig `restart==0 && flowPhase==0`) |
| P1-7 | 22 ExtrasGallery | SNK_CreditsText scroll roll | port reimplements credits as paged still-image slideshow |
| P1-8 | 23 HighScore | cheat-gated locked-cup-skip browse range | port wraps plain [0..0x19] |
| P1-9 | 1 PositionerDebugTool | glyph strip + 4-dir edit + positioner.txt writer; port also INVENTS Save/Back buttons | dev tool, low user impact |
| P1-10 | 3 LanguageSelect | 4 corner flag-image tiles + "LANGUAGE SELECT" title | port shows vertical text list; flow matches |

### P2 — Localization (pervasive, single root cause)

**All button/option labels are hardcoded English literals**, not resolved from `LANGUAGE.DLL` `SNK_*ButTxt` / `SNK_*Text` string tables. Screen 6 specifically renders a hardcoded English hover-description instead of localized `SNK_RaceTypeText`. This is the most widespread non-faithful trait but is one systemic fix (route labels through the SNK_ string tables) rather than 30 separate ones. The `SNK_*` symbol names are in `LANGUAGE.DLL` exports (see `reference_keyboard_rebind_applies_2026-05-30`).

### P3 — Cosmetic (non-functional)

Sprite-slide decorations, baked-vs-live surface differences, screen-4 fade-in-vs-out label, CarSelect tab-3 label "Stats" vs orig stem "Config". No behavioral impact.

### ARCH-DIV — Intentional / acceptable divergences (NOT bugs)

- Netplay (8/9/10/11/29) stubbed — no DXPTYPE peer layer.
- Screen 5 controller-required dialog = keyboard-first port choice.
- Screen 25 name entry via Win32 `GetAsyncKeyState` vs orig `DXInput::GetString` — equivalent input model.

## Recommended fix order

1. **P0 Options bugs (12, 14, 13, 15, 16, 19)** — these are real, user-visible, non-netplay, and each is a small localized edit. Biggest faithfulness win.
2. **P0-9 / P0-10 (25 highlight, 28 redirect)** — tiny, verify-then-fix.
3. **P2 localization** — one systemic change (SNK_ string routing); high visibility.
4. **P1 display-only gaps (20 Info panel, 22 credits, 23 range, 3 flags)** — cosmetic-leaning completeness.
5. **P1 netplay (8/9/10/11) + 18 joystick/wheel** — large, depends on DXPTYPE/DXInput decisions; treat separately.

Per-screen detail (button-by-button tables, element inventories, dispatch addresses, port refs, [UNCERTAIN] flags) follows.

---

# Per-screen behavior + parity detail

# Frontend behavior + parity harvest — behavior_00 (dispatch indices 0..4)

Source: TD5_d3d.exe via Ghidra pool slot TD5_pool10, read_only. Port: td5mod/src/td5re/td5_frontend.c.
Dispatch table g_frontendScreenFnTable @ 0x004655C4 → entries 0..4 = 0x004269d0, 0x00415030, 0x004275a0, 0x00427290, 0x004274a0.

GROUND TRUTH (cross-cutting):
- CreateFrontendDisplayModeButton @0x00425de0 — has 25 callers (full list captured); NONE of the 5 screens here is among them. The only sibling button helper used by any of these 5 is CreateFrontendMenuRectEntry @0x004258f0 (screen 3) which is a generic hit-rect/blit registrar, NOT the labelled menu-button helper. So NONE of these 5 screens creates a labelled frontend button.
- CreateFrontendMenuRectEntry @0x004258f0 sig __cdecl (x,y,w,h,src_x,src_y,user_data,surface): writes into g_connBrowserListOriginX_PROVISIONAL[] slot (origin_x/y, end_x/y = x+w / y+h, src offsets, user_data=param_7, surface, flags=0, select_progress=0). It IS a clickable rect (has hit-box + user_data + select_progress), used by screen 3 for the 4 language flags.
- Port has NO userdata/action field on buttons (frontend_create_button(label,x,y,w,h)); per-screen behavior is INDEX-coded in each Screen_* switch on s_button_index.

---

### Screen 0 @ 0x004269d0 — ScreenLocalizationInit  [interactive: N]
Port handler: td5_frontend.c:5983 (case TD5_SCREEN_LOCALIZATION_INIT, called via screen-fn table[0])
DISPATCH MODEL: none — boot/localization bootstrap, NOT a button screen. Three-state gate on g_frontendBootDispatchMode @ (mode 0 full-init, 1 re-entry, 2 resume-cup). Single guard `if (g_frontendInnerState != 0) return;`.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| — | (none) | — | — | — | n/a | — | No button creation calls at all. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| car-config string loader | mode-0 only: per car-ZIP reads "config.eng" entry, sscanf 17 tokens into DAT_0049b90c stride 0x330; `_` → space; falls back to SNK_ConfUnknown strings on read fail | gCarZipPathTable, SNK_Layout/Engine/Conf* exrefs (LANGUAGE.DLL) | Implemented (different layer) | td5_frontend.c:6027 td5_frontend_init_resources(); car stats via re/assets/cars/<car>/config.nfo (comment 6016-6019) | Port loads tokens from extracted config.nfo, not LANGUAGE.DLL config.eng; functionally equivalent, not byte-path-identical |
| display-mode enumerate/pick | mode-0: BuildEnumeratedDisplayModeList + FormatDisplayModeOptionStrings; if saved bindings mismatch, search 640x480x16 then best <640 width | gSelectedDisplayModeOrdinal, gConfiguredDisplayModeOrdinal, g_player2CustomBindings | Partial (delegated) | comment 6022 "handled in td5_render.c" | Mode-search/fallback to 640x480x16 not visibly reproduced in this handler |
| controller seed | mode-0: copies JoystickType/JoystickC tables to DAT_00465664/84; if no saved-vs-detected match → g_player1InputSource=0, g_player2InputSource=7, copy g_defaultControllerBindings | g_player1/2DeviceDesc, g_controllerBindings | Partial (delegated) | comment 6023-6025 "td5_input.c handles this" | DXInput/M2DX export seeding omitted; relies on td5_input.c |
| resume-cup volume restore | mode-2 only: g_replayFileAvailable=1, route RACE_RESULTS, re-quantize master+CD volume to nearest 10% | g_persistedMasterVolumePercent, g_persistedCdVolumePercent | Partial | td5_frontend.c:5988 sets s_results_skip_display=1 → RACE_RESULTS | Port routes to RACE_RESULTS but volume-percent re-quantize not in this handler |

PARITY VERDICT: Faithful (routing) — non-interactive bootstrap; control-flow gate mirrored 1:1. Heavy data-loading delegated to other modules (asset/render/input) by design.
GAPS (actionable):
- Port comment claims faithfulness but the config.eng→sscanf and display-mode fallback live in other modules; this handler is just the FSM skeleton. No on-screen elements to fix.
Confidence: [CONFIRMED @ 0x004269d0] zero button/draw-helper calls; gate + 3 modes verified. [UNCERTAIN] exact module that re-quantizes volume in port (not in this handler).

---

### Screen 1 @ 0x00415030 — ScreenPositionerDebugTool  [interactive: Y (dev tool)]
Port handler: td5_frontend.c:6047 (case TD5_SCREEN_POSITIONER_DEBUG, table[1])
DISPATCH MODEL: bit-mapped key input on g_frontendInputEdgeBits @0x00415030 (cases 3 & 4) — NOT button-index, NOT userdata, NOT callback. 0 callers besides the screen-fn table.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| — | (none) | — | — | — | Wrong-behavior | td5_frontend.c:6052-6053 | Original creates ZERO buttons. Port FABRICATES two: frontend_create_button("Save",120,400,96,32) and ("Back",232,400,112,32). These do not exist in 0x00415030. |

NON-BUTTON ELEMENTS (original):
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| glyph strip + caret | case3/case4 RenderPositionerGlyphStrip(0) draws font glyph cells + caret + selection marker | g_positionerGlyphRectsPrimary/Secondary, g_positionerSelectedGlyphIndex | Missing | — | Port has no glyph strip render; only frontend_load_tga("Positioner.tga") + present |
| glyph-select nav | case3: edge bits &1 (sel-1), &2 (sel+1), &0x200 (sel+8), &0x400 (sel-8) move g_positionerSelectedGlyphIndex | g_positionerSelectedGlyphIndex | Wrong-behavior | td5_frontend.c:6066 uses s_arrow_input/frontend_option_delta into s_anim_tick | Port nudges s_anim_tick, not a glyph index; semantics differ |
| glyph-rect editor | case4: edge bits &1/&2 adjust primary rect x; &0x200/&0x400 adjust secondary rect x of selected glyph | g_positionerGlyphRectsPrimary/Secondary[sel*2] | Missing | td5_frontend.c:6079 case4 edits s_anim_tick only | No per-glyph rect array edited in port |
| state advance key | cases 3/4: edge bit &0x40000 → g_frontendInnerState++ | g_frontendInputEdgeBits | Partial | implicit via state machine | Port advances on s_button_index, not the 0x40000 key bit |
| positioner.txt writer | case5: fopen "positioner.txt", dumps both glyph-rect arrays (0x25 rows × 7) then g_frontendInnerState=3 | g_positionerGlyphRectsPrimary/Secondary | Missing | td5_frontend.c:6091 only TD5_LOG_I then →MAIN_MENU | Port does not write positioner.txt |

PARITY VERDICT: Divergent — original is a dev glyph-positioner with no buttons; port reimplements it as a 2-button ("Save"/"Back") stub with no glyph strip, no rect editing, no file output. Screen is dev-only/unreachable in normal play, so impact is low.
GAPS (actionable):
- Port invents "Save"/"Back" buttons that the original lacks (td5_frontend.c:6052-6053).
- Port omits the glyph strip render, the 4-direction glyph/rect editing on g_frontendInputEdgeBits, and the positioner.txt writer.
Confidence: [CONFIRMED @ 0x00415030, helper 0x00414f40] no CreateFrontend*Button call; input is purely g_frontendInputEdgeBits.

---

### Screen 2 @ 0x004275a0 — RunAttractModeDemoScreen  [interactive: N]
Port handler: td5_frontend.c:6117 (case TD5_SCREEN_ATTRACT_MODE, table[2])
DISPATCH MODEL: none — linear 6-state pump (switch g_frontendInnerState 0..5), no input read, no buttons.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| — | (none) | — | — | — | n/a | — | No button creation. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| attract flag + present | case0: g_attractModeDemoActive=1, Present, ActivateFrontendCursorOverlay | g_attractModeDemoActive @0x495254 | Implemented | td5_frontend.c:6122 s_attract_demo_active=1 + present + cursor(0) | — |
| button release | case1: ReleaseFrontendDisplayModeButtons | (button pool) | Implemented | td5_frontend.c:6130 frontend_reset_buttons | — |
| present x2 | case2/3: PresentPrimaryFrontendBufferViaCopy | — | Implemented | td5_frontend.c:6135/6140 | — |
| fade-to-black + launch | case4 InitFrontendFadeColor(0); case5 RenderFrontendFadeEffect → on done InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState | g_frontendFadeActive | Implemented | td5_frontend.c:6145/6150 frontend_init_fade + render_fade + init_race_schedule + init_display_mode_state | — |

PARITY VERDICT: Faithful — case-for-case 6-state mirror; no interactive elements.
GAPS (actionable): none.
Confidence: [CONFIRMED @ 0x004275a0] no placement/button/input calls.

---

### Screen 3 @ 0x00427290 — ScreenLanguageSelect  [interactive: Y (no labelled buttons)]
Port handler: td5_frontend.c:6164 (case TD5_SCREEN_LANGUAGE_SELECT, table[3])
DISPATCH MODEL: index-via-hover @0x00427290 case 4 — reads g_frontendButtonPressedFlag + g_frontendButtonIndex, but the ONLY action on ANY press is advance state → screen always exits to LEGAL_COPYRIGHT. It does NOT branch on which flag, and writes NO language global. Selection is effectively a NO-OP in the original.

BUTTONS / clickable rects (original order — via CreateFrontendMenuRectEntry @0x004258f0, all userdata=0):
| # | label (graphic) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | flag tile src_y=0 (top-left) | 0 | none beyond "press → advance → LEGAL" | none | Wrong-behavior | td5_frontend.c:6170 "English" | Port uses TEXT button labels not flag tiles; positions differ (120,180 vs canvas-corner) |
| 2 | flag tile src_y=0x80[128] (top-right) | 0 | same | none | Wrong-behavior | td5_frontend.c:6171 "French" | label/position speculative |
| 3 | flag tile src_y=0x100[256] (bot-left) | 0 | same | none | Wrong-behavior | td5_frontend.c:6172 "German" | label/position speculative |
| 4 | flag tile src_y=0x180[384] (bot-right) | 0 | same | none | Partial/Wrong | td5_frontend.c:6173 "Spanish" | Orig has 4 flag tiles; ScreenLocalizationInit lists 5 langs (Eng/Fr/Ger/Ita/Spa) — port's 4 labels guess which 4 |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| title "LANGUAGE SELECT" | case0 DrawFrontendLocalizedStringPrimary literal @0x004667c0 top-center | — | Partial | not drawn as title text; baked into screen logic | Port draws 4 button labels but no "LANGUAGE SELECT" title row |
| hover anim counter | case4: if g_frontendButtonIndex != hoverIdx → g_frontendAnimFrameCounter=0 (hover highlight reset) | g_frontendAnimFrameCounter, g_languageSelectButtonHoverIndex_PROVISIONAL | Partial | td5_frontend.c:6178-6189 s_anim_tick ramp | Port has a generic anim ramp, not the hover-change reset semantics |
| selection result | none — selected index never written to a language global | (none) | Faithful-by-accident | td5_frontend.c:6193 stores s_flow_context=index then ignored | Port also stores-then-ignores; net behavior matches (selection is cosmetic) |

PARITY VERDICT: Divergent (cosmetic) — functionally the screens agree (any click → LEGAL, language not applied), but the port renders 4 text buttons in a vertical list instead of 4 flag-image tiles at canvas corners, omits the "LANGUAGE SELECT" title, and its 4 labels (Eng/Fr/Ger/Spa) are a guess that may drop Italian.
GAPS (actionable):
- Port should render 4 flag image tiles (Language.tga rows at src_y 0/128/256/384, 176x128) at the 4 canvas corners, not a vertical text list (td5_frontend.c:6170-6173).
- Port omits the "LANGUAGE SELECT" title string @0x004667c0.
- Port's 4 language labels are unverified; orig flag count=4 but ScreenLocalizationInit enumerates 5 languages — which 4 flags map to which language is [UNCERTAIN] (Language.tga tile contents not decoded here).
- Both correctly treat the selection as inert (no language global written), so no behavioral regression on that axis.
Confidence: [CONFIRMED @ 0x00427290] 4 CreateFrontendMenuRectEntry calls userdata=0; no language global written; title literal "LANGUAGE SELECT". [UNCERTAIN] which language each flag tile depicts (image not decoded).

---

### Screen 4 @ 0x004274a0 — ScreenLegalCopyright  [interactive: N]
Port handler: td5_frontend.c:6218 (case TD5_SCREEN_LEGAL_COPYRIGHT, table[4]); overlay td5_frontend.c:5572 → frontend_render_legal_copyright_overlay (5292)
DISPATCH MODEL: none — timed splash, switch g_frontendInnerState 0..3, no input read, no buttons.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| — | (none) | — | — | — | n/a | — | No button creation. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| tiled copyright text | case0: loop (g_frontendCanvasH-32)/32 rows of DrawFrontendLocalizedStringSecondary at x=W/10; "TEST DRIVE 5 COPYRIGHT 1998" @0x00466808 | — | Implemented | td5_frontend.c:5292 frontend_render_legal_copyright_overlay | y per-row recovered in port (orig decompiled y is a denormal float misdecode) |
| LegalScreen.tga bg | case0 LoadTgaToFrontendSurface16bppVariant | — | Implemented | td5_frontend.c:6222 frontend_load_tga | — |
| fade-out + 3s dwell + fade + exit | case1 fade-out + capture timeGetTime; case2 wait >2999ms; case3 fade then SetFrontendScreen(MAIN_MENU) | g_frontendFadeActive, g_frontendInnerState(=timestamp) | Implemented | td5_frontend.c:6232 fade-in ramp, 6242 `>2999u` dwell, 6248 fade-out → MAIN_MENU | Port case1 is "fade in" (anim ramp) vs orig "fade out"; dwell + exit faithful |

PARITY VERDICT: Faithful — timed copyright splash; tiled text + 3s dwell + exit-to-menu all mirrored.
GAPS (actionable):
- Minor: port case1 labels its ramp "fade in" while orig case1 is RenderFrontendFadeOutEffect; both gate on completion before the 2999ms dwell, so visible result matches. Worth a label fix only.
Confidence: [CONFIRMED @ 0x004274a0] dwell=2999ms, exit MAIN_MENU, tiled draw via DrawFrontendLocalizedStringSecondary; [UNCERTAIN] orig per-row y (decompiled as denormal 6.465804e-39 — FP-stack misdecode), port supplies its own per-row y.

---

## Cross-screen summary
- Interactive: 0=N, 1=Y(dev), 2=N, 3=Y(no labelled buttons), 4=N.
- Real labelled frontend buttons (CreateFrontendDisplayModeButton) in originals: 0 across all 5.
- Screen 3 clickable rects: 4 flag tiles (userdata all 0, selection inert).
- Missing/Wrong port items: screen1 = 2 fabricated buttons + glyph strip/editor/file-out missing (≈6 items); screen3 = 4 flag-tiles rendered as text buttons + title missing (≈5 items). Screens 0/2/4 = no Missing/Wrong (Partial-by-delegation only on 0).
- Top gaps: (1) port screen1 invents Save/Back buttons absent in orig; (2) port screen3 shows a vertical text list instead of 4 corner flag-image tiles + drops the "LANGUAGE SELECT" title; (3) port screen1 lost the glyph positioner functionality (dev-only, low impact).
- [UNCERTAIN] flags: orig screen4 per-row y (FP misdecode); which language each of the 4 flag tiles depicts (Language.tga not decoded); orig screen0 5-vs-4 language enumeration vs screen3's 4 tiles.
# Frontend Behavior Harvest — Part 05 (screen indices 5–9)

Source: TD5_d3d.exe via Ghidra (read-only, slot TD5_pool10). Port: `td5mod/src/td5re/td5_frontend.c`.
Position harvest companion: `part_05.md` (draw inventory). This file = BEHAVIOR + parity.

Shared dispatch facts:
- Original button factory `CreateFrontendDisplayModeButton @0x00425de0(label,x,y,w,h,userdata)`. **Every call in these 5 screens passes userdata = 0** (literal 6th arg `0` in every decomp call). The original does NOT dispatch on userdata — it dispatches on **`g_frontendButtonIndex`** (insertion order index) compared/switched in the interaction state. So the port's index-switch model is faithful to the original's model; userdata is dead in these screens.
- Greyed/disabled variant = `CreateFrontendDisplayModePreviewButton` (same signature). Port equivalent = `frontend_create_preview_button(...)`.
- `g_frontendButtonPressedFlag` = press edge; `g_frontendButtonIndex` = hovered/selected index; `g_frontendEscKeyButtonIndex` = which button ESC maps to.
- Port screen enum (td5_types.h:312): MAIN_MENU=5, RACE_TYPE_MENU=6, QUICK_RACE=7, CONNECTION_BROWSER=8, SESSION_PICKER=9, CREATE_SESSION=10, NETWORK_LOBBY=11, OPTIONS_HUB=12, CONTROL_OPTIONS=14, CAR_SELECTION=20, EXTRAS_GALLERY=22, HIGH_SCORE=23, RACE_RESULTS=24. `~TD5_SCREEN_LOCALIZATION_INIT == ~0 == -1` = "launch race / restart race flow" sentinel.

---

### Screen 5 @ 0x00415490 — ScreenMainMenuAnd1PRaceFlow  [interactive: Y]
Port handler: td5_frontend.c:6265 (Screen_MainMenu, case TD5_SCREEN_MAIN_MENU)
DISPATCH MODEL: index-switch @0x004155F0 (case 4 `if (g_frontendButtonPressedFlag != 0) switch(g_frontendButtonIndex)`) — sets g_returnToScreenIndex + g_mainMenuButtonHint_PROVISIONAL then slides out (state 8→9) and SetFrontendScreen(g_returnToScreenIndex). Button 6 (Exit) opens an in-place Yes/No sub-dialog (state 4→5→6). Port mirrors with `s_button_index` switch.

BUTTONS (original order — all userdata=0):
| # | label (SNK_/literal) | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_RaceMenuButTxt ("Race Menu") | 0 | g_returnToScreenIndex=RACE_TYPE_MENU(6); hint=1; →state8 slide-out (0x415C0E) | none | Implemented | :6343 case 0 | none |
| 1 | SNK_QuickRaceButTxt ("Quick Race") | 0 | g_returnToScreenIndex=QUICK_RACE(7); hint=2 (0x415C28) | none | Implemented | :6349 case 1 | Port also calls ConfigureGameTypeFlags()+game_type=0 here (port addition; orig does NOT — orig screen-7 sets game_type=-1 in its own init). Behavior benign (screen 7 re-inits) |
| 2 | SNK_TwoPlayerButTxt ("Two Player") OR SNK_TimeDemoButTxt | 0 | if `*(app+0x170)!=0`: benchmark path (g_benchmarkRequestPending=1, InitializeRaceSeriesSchedule, InitializeFrontendDisplayModeState, return). else: g_returnToScreenIndex=CAR_SELECTION(20); g_twoPlayerModeEnabled=1; g_frontendSelectedGameType=0; hint=3 (0x415C3C) | label & branch gated on `*(g_appExref+0x170)` (never written in binary → always TwoPlayer) | Implemented (ARCH-DIVERGENCE) | :6367 case 2 | Port label hardcoded "Two Player"; benchmark path exposed via INI `enable_benchmark` instead of app+0x170. Faithful given app+0x170 is dead in shipped binary |
| 3 | SNK_NetPlayButTxt ("Net Play") | 0 | g_returnToScreenIndex=CONNECTION_BROWSER(8); hint=4; blits exit-dialog region then →state8 (0x415C5A) | none | Implemented | :6401 case 3 | none |
| 4 | SNK_OptionsButTxt ("Options") | 0 | g_returnToScreenIndex=OPTIONS_HUB(12); hint=5 (0x415CB6) | none | Implemented | :6407 case 4 | none |
| 5 | SNK_HiScoreButTxt ("High Scores") | 0 | g_returnToScreenIndex=HIGH_SCORE(23); hint=6 (0x415CCA) | none | Implemented | :6413 case 5 | none |
| 6 | SNK_ExitButTxt ("Exit") | 0 | opens Yes/No sub-dialog: BeginFrontendDisplayModePreviewLayout, creates SNK_Yes/SNK_Nox buttons, →state5 (0x415CDE). EscKeyButtonIndex=6 | none | Implemented | :6419 case 6 →state5 | none |
| 7 (dlg) | SNK_YesButTxt ("Yes") | 0 | state6: index==0 → state10 (scatter) → SetFrontendScreen(EXTRAS_GALLERY=22) i.e. credits/quit (0x415F33) | sub-dialog only | Partial | :6431/6446/6458 | Port "Yes" → SCREEN_EXTRAS_GALLERY then frontend_post_quit (state7/10). Orig case 0xc routes EXTRAS_GALLERY as the credits-roll exit. Acceptable |
| 8 (dlg) | SNK_NoxButTxt ("No") | 0 | state6: index==1 → release dialog buttons, restore layout, EscKeyButtonIndex=5, →state7→state4 (0x415F5B) | sub-dialog only | Implemented | :6447 (No → trims buttons to 7, refocus 6, state4) | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Header/title label | CreateMenuStringLabelSurface(0) blit; slides in state3, static state4 | g_currentScreenIndex (surface handle) | Implemented | title-tex path :5783 | Rendered via per-screen title texture |
| 7 button slide sprites (slot 0..6) | MoveFrontendSpriteRect intro(3)/outro(9)/extras-outro(0xc) animation | sprite slots | Implemented | timed-animation :6326/:6470 | Anim positions approximated by frontend_update_timed_animation, not per-slot orig formula |
| "Exit?" sub-caption | QueueFrontendOverlayRect of g_oneRaceFlowSelectedCarId (CreateMenuStringLabelSurface(8)) in state 5/6/10 | g_oneRaceFlowSelectedCarId | [UNCERTAIN port] | — | Port shows a generic Yes/No dialog; the localized "Exit?" caption surface (orig CreateMenuStringLabelSurface(8)) not separately drawn |
| "Must select controller" dialog (states 0x14-0x17) | SNK_MustSelectTxt + SNK_MustPlayer1/2Txt; OK button; triggered when input source==7 (no joystick) | g_player1/2InputSource, g_frontendMustSelectMessage | Missing (ARCH-DIVERGENCE) | :6472 note | Intentionally dropped — port is keyboard-first, joystick==7 gate unreachable |

PARITY VERDICT: Faithful — all 7 buttons + Yes/No dispatch correct; only the controller-required dialog (input-source gated) and the localized "Exit?" caption surface are absent (both intentional/keyboard-first).
GAPS (actionable):
- "Must select controller" dialog (orig states 0x14-0x17) absent — intentional ARCH-DIVERGENCE, document only.
- Button-2 label is hardcoded "Two Player"; benchmark branch moved to INI (app+0x170 is dead in binary, so faithful).
- Port adds ConfigureGameTypeFlags() on Quick-Race press (orig does not); benign because screen 7 re-inits game_type.
Confidence: [CONFIRMED @ 0x00415490 case-4 switch, dispatch addrs 0x415C0E..0x415F5B] / [UNCERTAIN: exact "Exit?" caption surface presence in port render path]

---

### Screen 6 @ 0x004168b0 — RaceTypeCategoryMenuStateMachine  [interactive: Y]
Port handler: td5_frontend.c:6528 (Screen_RaceTypeCategory, case TD5_SCREEN_RACE_TYPE_MENU)
DISPATCH MODEL: index-switch, TWO pages.
- Top page @0x00416B3D (state 3 `if (g_frontendButtonPressedFlag) switch(g_frontendButtonIndex)`).
- Cup sub-page @0x00417230 (state 8 `if (g_frontendButtonPressedFlag && idx>=0)`; idx<6 → game_type=idx+1, idx==6 → Back).
- Description preview redrawn on hover-change (state 3→4 / 8→9) into g_lobbyErrorDialogSurface.

BUTTONS — TOP PAGE (states 0/0xb; original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_SingleRaceButTxt ("Single Race") | 0 | g_frontendSelectedGameType=0; ConfigureGameTypeFlags(); g_returnToScreenIndex=CAR_SELECTION(20); →state5 (0x416B5F) | none | Implemented | :6585 case 0 | none |
| 1 | SNK_CupRaceButTxt ("Cup Race") | 0 | →state6 (cup sub-menu) (0x416BBD) | none | Implemented | :6592 case 1 →state6 | none |
| 2 | SNK_ContCupButTxt ("Continue Cup") | 0 | LoadContinueCupData(); g_replayFileAvailable=0; g_returnToScreenIndex=RACE_RESULTS(24); →state5 (0x416BCF) | **greyed (Preview) if `ValidateCupDataChecksum()==0`** | Implemented | :6552 (preview gate) / :6596 case 2 | Port: if valid → load+go RACE_RESULTS; else play sfx(10). Matches |
| 3 | SNK_TimeTrialsButTxt ("Time Trials") | 0 | g_frontendSelectedGameType=9 (orig enum: 9=TimeTrial); ConfigureGameTypeFlags(); →CAR_SELECTION (0x416B9D via i+3 override) | none | Implemented (enum remap) | :6606 case 3 → port game_type=7 | Port uses port-enum 7=TIME_TRIAL (deliberate value remap, see :6506 note). Semantics preserved |
| 4 | SNK_DragRaceButTxt ("Drag Race") | 0 | g_frontendSelectedGameType=7 (orig 7=Drag); →CAR_SELECTION (0x416B90, i+3=7) | none | Implemented (enum remap) | :6613 case 4 → port game_type=9 | Port-enum 9=DRAG_RACE. Semantics preserved |
| 5 | SNK_CopChaseButTxt ("Cop Chase") | 0 | g_frontendSelectedGameType=8 (orig 8=CopChase); →CAR_SELECTION (0x416B90, i+3=8) | none | Implemented (enum remap) | :6621 case 5 → port game_type=8 | Matches |
| 6 | SNK_BackButTxt ("Back", narrow w=0x70) | 0 | g_returnToScreenIndex = (g_networkLobbyEntryPhase==1 ? CREATE_SESSION(10) : MAIN_MENU(5)); →state5 (0x416BFA) | none; EscKeyButtonIndex=6 | Implemented | :6628 case 6 | Port: `s_network_active ? CREATE_SESSION : MAIN_MENU`. Matches (gate var renamed) |

BUTTONS — CUP SUB-PAGE (state 6 init; original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating (orig `g_cheatFlagBitfieldGameModes`) | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChampionshipButTxt | 0 | state8: idx<6 → g_frontendSelectedGameType=idx+1=1; g_raceWithinSeriesIndex=0; ConfigureGameTypeFlags(); g_selectedScheduleIndex=g_attractModeTrackIndex; g_returnToScreenIndex=CAR_SELECTION; →state10 (0x417230) | always normal | Implemented | :6696 cup_type=1 | none |
| 1 | SNK_EraButTxt | 0 | idx+1=2 → game_type=2 (0x417230) | always normal | Implemented | :6697 cup_type=2 | none |
| 2 | SNK_ChallengeButTxt | 0 | game_type=3 | **Preview(greyed) if cheatGameModes==0; normal if >=1** | Implemented | :6651 gate / :6698 | Port gate var `s_cup_unlock_tier>=1`. Matches threshold |
| 3 | SNK_PitbullButTxt | 0 | game_type=4 | **Preview if cheatGameModes==0; normal if >=1** | Implemented | :6657 / :6702 | Matches |
| 4 | SNK_MastersButTxt | 0 | game_type=5 | **Preview unless cheatGameModes>=2** | Implemented | :6663 / :6706 | Matches |
| 5 | SNK_UltimateButTxt | 0 | game_type=6 | **Preview unless cheatGameModes>=2** | Implemented | :6669 / :6710 | Matches |
| 6 | SNK_BackButTxt (absolute pos) | 0 | state8 idx==6 → →state0xb (rebuild top page) (0x4172B6) | EscKeyButtonIndex=6 | Implemented | :6714 case 6 →state11 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Description box (0x110×0xb4) | g_lobbyErrorDialogSurface; on hover-change (state4/9) blits **localized** SNK_RaceTypeText[selectedGameType] title (large) + wrapped body (small font, y=0x20 step 0xc) | g_frontendSelectedGameType indexes SNK_RaceTypeText (orig 0x416C..) | **Wrong-source** | frontend_render_race_type_description :4158, dispatch :5522 | Port draws **hardcoded English** lines (k_race_type_lines/k_cup_lines), NOT the localized LANGUAGE.DLL SNK_RaceTypeText strings. Diverges from original text + breaks localization |
| Header/title label | CreateMenuStringLabelSurface(1) | g_currentScreenIndex | Implemented | title-tex :5783 | none |
| Button slide sprites slot0..6 | intro/outro MoveFrontendSpriteRect | sprite slots | Implemented | timed-animation | Anim positions approximated |

PARITY VERDICT: Partial — all 13 buttons (top 7 + cup 7, one shared Back) and full dispatch + gating are faithful (incl. enum-value remap and cheat-tier gating). The hover description panel text is the one real divergence: port renders fabricated English instead of localized SNK_RaceTypeText.
GAPS (actionable):
- Description preview uses hardcoded English text, not localized `SNK_RaceTypeText[game_type]` (LANGUAGE.DLL). Wrong content + no localization. (1 Wrong-behavior)
- Port `s_cup_unlock_tier` is the mapped equivalent of orig `g_cheatFlagBitfieldGameModes`; thresholds (>=1 / >=2) match — verify the source global is fed identically.
Confidence: [CONFIRMED @ 0x004168b0 state-3 switch 0x416B3D, state-8 switch 0x417230, cheatGameModes gating 0x417096-0x4171C7, SNK_RaceTypeText index expr] / [UNCERTAIN: whether s_cup_unlock_tier is sourced from the same cheat bitfield as orig 0x...GameModes]

---

### Screen 7 @ 0x004213d0 — ScreenQuickRaceMenu  [interactive: Y]
Port handler: td5_frontend.c:6762 (Screen_QuickRaceMenu, case TD5_SCREEN_QUICK_RACE)
DISPATCH MODEL: index-switch @0x00421A61 (state 4). Buttons 0/1 are **◄► selectors** (cycle car/track via g_postRaceRacerCardNavDirection, NOT a press-dispatch); buttons 2/3 are press buttons (OK/Back via g_frontendButtonPressedFlag). OK launches race (g_returnToScreenIndex = -1 sentinel → state6 → InitializeRaceSeriesSchedule).

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChangeCarButTxt ("Change Car", w=0x100 selector) | 0 | selector: on g_postRaceRacerCardNavDirection!=0 → g_quickRaceSelectedTrackId += dir (this is the CAR index here), wrap by g_savedMaxUnlockedCar (or 0x21/0x25 in cheat), redraw car name into panel (0x4218A2) | wrap bounds gated by g_cheatPostRaceHighScoreUnlock / g_cheatAttractModeOverride / DAT_00463e6d | Implemented | :6786 is_selector / :6816 cycle car | Port wraps via s_total_unlocked_cars (or 32 cheat / 36 net). Names differ but logic equivalent |
| 1 | SNK_ChangeTrackButTxt ("Change Track", selector) | 0 | selector: g_selectedScheduleIndex += dir, wrap by g_savedMusicTrackIndex (or 0x13 cheat), redraw track name (0x42191C) | cheat-gated wrap | Implemented | :6788 / :6827 cycle track | Port wraps via s_total_unlocked_tracks (or 0x13 net). Equivalent |
| 2 | SNK_OkButTxt ("OK") | 0 | press: if car/track locked && !cheat → DXSound::Play(10) reject; else g_selectedCarIndex=g_quickRaceSelectedTrackId; paint=0; wheel=0; g_returnToScreenIndex=-1 (~LOCALIZATION_INIT); →state5 (0x421A6E) | locked-content gate | Implemented | :6840 case 2 | none |
| 3 | SNK_BackButTxt ("Back") | 0 | press: g_selectedCarIndex=...; g_returnToScreenIndex=MAIN_MENU(5); →state5 (0x421B0C). EscKeyButtonIndex=3 | none | Implemented | :6855 case 3 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Info panel (0x208×200) | g_lobbyErrorDialogSurface; holds car name (top half), track name (bottom half), "Locked" caption when locked | g_quickRaceSelectedTrackId (car), g_selectedScheduleIndex (track) | Implemented | frontend_render_quick_race_overlay :3928, dispatch :5525 | Port draws car/track names + "LOCKED" live (no offscreen surface). Text source = port name tables, not the orig surface blit, but content correct |
| ◄► arrow sprites on btn 0/1 | InitializeFrontendDisplayModeArrows(0,1)/(1,1); ArrowButtonz cycle arrows | selector widgets | Implemented | fe_draw_option_arrows :3948, dispatch :5717 | none |
| Header/title label | CreateMenuStringLabelSurface(3) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Faithful — 2 selectors + OK + Back all correct, lock-gating on OK present, race-launch sentinel (-1) handled. Panel content drawn live instead of via offscreen surface (rendering-arch difference, content equivalent).
GAPS (actionable):
- None functional. Panel/name text comes from port tables vs orig localized surface blit (cosmetic/localization, same as other screens).
Confidence: [CONFIRMED @ 0x004213d0 state-4 dispatch 0x421A61, selector handling 0x42188C/0x42191C, OK launch sentinel 0x421A6E]

---

### Screen 8 @ 0x00418d50 — RunFrontendConnectionBrowser  [interactive: Y]  [ARCH-DIVERGENCE: DXPTYPE]
Port handler: td5_frontend.c:6888 (Screen_ConnectionBrowser, case TD5_SCREEN_CONNECTION_BROWSER)
DISPATCH MODEL: index-switch @0x004191D0 (state 5) for the 3 buttons; a SEPARATE list-navigation sub-state @0x00419240 (state 6) handles up/down scroll of the provider list (g_frontendInputEdgeBits 0x200=up / 0x400=down, mouse-row hit-test). Original enumerates DirectPlay connection providers (DXPlay::ConnectionEnumerate) into `dpu_exref+0x1e0` count, draws up to 4 rows into the panel surface.

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChooseConnectionButTxt ("Provider"/list panel, w=0x1f0 h=0x80) | 0 | press idx==0 → state6 (enter list-scroll mode) (0x4191E8) | none | Partial | :6896 "Provider" / :6926 case 0 | Port: idx0+arrow → sfx(2) only. NO scrollable provider list (state 6/7 collapsed to no-op → state5, :6944). DirectPlay enumeration absent |
| 1 | SNK_OkButTxt ("OK") | 0 | press idx==1 → g_returnToScreenIndex=9 (SESSION_PICKER); →state8 slide-out (0x4191F8) | none | Implemented | :6928 case 1 | none |
| 2 | SNK_BackButTxt ("Back") | 0 | press idx==2 → g_returnToScreenIndex=5 (MAIN_MENU); →state8 (0x419208). EscKeyButtonIndex=2 | none | Implemented | :6931 case 2 | none |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Provider list rows (≤4) | DrawFrontendClippedStringToSurface from `dpu_exref + (scrollOff+i)*0x3c`, into panel surface (x=0x20, y step 0x10) | dpu_exref+0x1e0 (provider count), g_connBrowserScrollOffset | **Missing** | (no render case for screen 8 in :5520 switch) | Port draws no provider rows; the panel is an empty "Provider" button. No DirectPlay providers exist in port |
| Up/Down scroll arrows | g_browserScrollIndicatorSprite at panel origin +0xf2 / +0x1e (up if scrollOff!=0), +0x6c (down if scrollOff+4<count) | g_connBrowserScrollOffset, count | Missing | — | No list → no scroll indicators |
| Selection bar | g_browserSelectionBarSprite at row (cursor-scroll)*0x10+0x2f; blinks (anim&0x10) in state6 | g_connBrowserCursorIndex | Missing | — | No list → no selection bar |
| Header/title label | CreateMenuStringLabelSurface(5) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Partial (ARCH-DIVERGENCE) — OK/Back navigation faithful; the entire DirectPlay provider-list browser (rows, scroll arrows, selection bar, list-scroll sub-state 6) is absent because the port has no DXPTYPE peer enumeration. Button 0's press just plays a sound.
GAPS (actionable):
- No provider list, scroll indicators, or selection bar (3 Missing elements) — DXPTYPE ARCH-DIVERGENCE, documented.
- Button-0 list-scroll sub-state (orig state 6/7) collapsed to no-op (:6944). Document only.
Confidence: [CONFIRMED @ 0x00418d50 state-5 dispatch 0x4191D0, list-scroll 0x419240, DXPlay::ConnectionEnumerate @ case 0]

---

### Screen 9 @ 0x00419cf0 — RunFrontendSessionPicker  [interactive: Y]  [ARCH-DIVERGENCE: DXPTYPE]
Port handler: td5_frontend.c:6967 (Screen_SessionPicker, case TD5_SCREEN_SESSION_PICKER)
DISPATCH MODEL: index-switch @0x00419E80 (state 3) for 3 buttons (orig has NO separate "Create" button — slot0 panel + OK + Back). List-scroll handled in state 4 @0x00419F50 (same 0x200/0x400 edge model as screen 8). Sessions enumerated via RenderFrontendSessionBrowser into dpu_exref+0xbfc (session count).

BUTTONS (original order, all userdata=0):
| # | label | userdata | action (orig addr) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ChooseSessionButTxt ("Session"/list panel, w=0x1f0 h=0x80) | 0 | press idx==0 → state4 (list-scroll mode) (0x419EA8) | none | Partial | :6973 "Session" / :6994 case 0 | Port: idx0+arrow → sfx(2). No scrollable session list (RenderFrontendSessionBrowser absent) |
| 1 | SNK_OkButTxt ("OK") | 0 | press idx==1 → `*(dpu_exref+0xc00)=cursorIdx-1` (selected session); g_returnToScreenIndex=CREATE_SESSION(10); DXPlay::EnumSessionTimer(0); →state6 (0x419EC0) | none | **Partial/Wrong-target** | :6996 case 1 "Create"→state4 / :6998 case 2 "OK"→CREATE_SESSION | Port adds a 4th "Create" button NOT in orig; OK is port's button index 2. Both route to CREATE_SESSION. Orig OK=join-selected-session (writes selected idx); port writes no session index (no list). Functionally collapses to "go create/lobby" |
| 2 | SNK_BackButTxt ("Back") | 0 | press idx==2 → g_returnToScreenIndex=CONNECTION_BROWSER(8); DXPlay::EnumSessionTimer(0); →state6 (0x419EE8). EscKeyButtonIndex=2 | none | Implemented | :7001 case 3 (port idx3) "Back" → CONNECTION_BROWSER | Matches target; port index shifted by the added Create button |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Session list rows | RenderFrontendSessionBrowser() into panel surface (called state1/2/3/4) | dpu_exref+0xbfc (session count) | **Missing** | (no render case for screen 9 in :5520 switch) | Port draws no session rows |
| Up/Down scroll arrows | g_browserScrollIndicatorSprite at panel origin +0xf2/+0x1e (up), +0x6c (down) | g_connBrowserScrollOffset | Missing | — | No list |
| Selection bar | g_browserSelectionBarSprite at (cursor-scroll)*0x10+0x2f; blink anim&0x10 (state4) | g_connBrowserCursorIndex | Missing | — | No list |
| Header/title label | uses g_currentScreenIndex from screen 8 (not re-created here) | g_currentScreenIndex | Implemented | title-tex :5783 | none |

PARITY VERDICT: Partial/Divergent (ARCH-DIVERGENCE) — Back navigation faithful; OK collapses to "create/lobby" without selecting an enumerated session; the full session-list browser (rows, scroll, selection bar, RenderFrontendSessionBrowser) is absent (DXPTYPE). Port also adds a non-original "Create" button, shifting indices.
GAPS (actionable):
- Port adds a "Create" button (idx1) absent in the original 3-button (panel/OK/Back) layout — index shift; both Create and OK route to CREATE_SESSION. (1 Wrong-behavior: extra button + OK no longer joins a selected session)
- Session list rows / scroll arrows / selection bar absent (3 Missing) — DXPTYPE ARCH-DIVERGENCE.
- Orig OK writes selected session index (`dpu+0xc00 = cursor-1`) + stops EnumSessionTimer; port does neither (no list to select from).
Confidence: [CONFIRMED @ 0x00419cf0 state-3 dispatch 0x419E80, list-scroll 0x419F50, RenderFrontendSessionBrowser calls, dpu+0xc00/0xbfc session globals]

---

## Cross-screen summary
- Original dispatch model for ALL 5 screens = index-switch on `g_frontendButtonIndex` with `g_frontendButtonPressedFlag` edge (userdata arg is literal 0 everywhere → unused). Port's `s_button_index` switch is structurally faithful.
- Screens 5/6/7 = fully interactive single-player menus, near-faithful. Screens 8/9 = network browsers, deliberately gutted (DXPTYPE).
- The recurring REAL (non-divergence) gap: **Screen 6 description panel renders hardcoded English instead of localized SNK_RaceTypeText** — only genuine wrong-behavior bug found in the interactive trio.
# Frontend Behavior Harvest — Part 10 (screen-table indices 10–14)

Source: `TD5_d3d.exe` via Ghidra (read-only), full-function decompile of each entry.
Companion to `part_10.md` (POSITIONS). This file = BEHAVIOR + PORT PARITY.

Common dispatch facts:
- All 5 screens are FSMs on `g_frontendInnerState` (port mirror: `s_inner_state`).
- Original button click model: `BeginFrontendDisplayModePreviewLayout`/`CreateFrontendDisplayModeButton` assigns each button an implicit ORDER index. The runtime sets `g_frontendButtonIndex` (= which button is highlighted/clicked) and `g_frontendButtonPressedFlag` (= click) elsewhere (input dispatcher). Screen code reads `g_frontendButtonIndex` in a `switch`/`if`-ladder → **INDEX-based dispatch, NO userdata** (every CFDMB userdata arg = literal `0` on all 5 screens). Option arrows set `g_postRaceRacerCardNavDirection` (= -1/+1), read by option screens (13/14) per row.
- Port mirror: `frontend_create_button(label,x,y,w,h)` (no userdata), dispatch via `switch(s_button_index)` / `s_selected_button`; arrows via `frontend_option_delta()`. Port `s_button_index` ↔ orig `g_frontendButtonIndex`; port `frontend_option_delta()` ↔ orig `g_postRaceRacerCardNavDirection`.
- ESC handling: orig writes `g_frontendEscKeyButtonIndex` = index of the Back/OK button. Port has equivalent `s_button_index`==back path.

---

### Screen 10 @ 0x0041a7b0 — RunFrontendCreateSessionFlow  [interactive: Y]
Port handler: td5_frontend.c:7034 (Screen_CreateSession, case TD5_SCREEN_CREATE_SESSION)
DISPATCH MODEL: index-switch @0x0041a7b0 — `switch(g_frontendInnerState)` (DXPlay name-entry/join FSM); per-state buttons read `g_frontendButtonIndex` + name editor `g_postRaceNameEditState`.

This is NOT a row-of-options screen. It is a text-entry + DirectPlay session-create/join state machine. Buttons appear only in entry states.

BUTTONS (original order):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_EnterNewSessionNameButTxt ("ENTER NEW SESSION NAME") | 0 | name-entry field button (state 0); target buf DAT_00497068 max 0x10; confirm (g_postRaceNameEditState==2) → advance to join flow | state 0 only, when g_connBrowserCursorIndex==0 | Partial | :7040 frontend_create_button("Enter Name",...) | port label "Enter Name" not SNK string; collapses both session-name and player-name into one input |
| 2 | SNK_BackButTxt ("BACK") | 0 | btn idx 1 + pressed → g_returnToScreenIndex=SESSION_PICKER, slide out | state 0 | Implemented | :7041 frontend_create_button("Back",...); :7058 if(s_button_index==1)→SESSION_PICKER | faithful target |
| 3 | SNK_EnterPlayerNameButTxt ("ENTER PLAYER NAME") | 0 | player-name field (states 4 & 0x10); target g_postRacePlayerNameEntryBuffer max 0x10 | state 4 (from session-create path) / state 0x10 (g_connBrowserCursorIndex!=0 join path) | Missing | — | port has no separate player-name entry state; states 4-15 collapsed (ARCH-DIVERGENCE DXPTYPE @:7074) |
| 4 | SNK_BackButTxt ("BACK") | 0 | back from player-name (states 4/0x10) → g_returnToScreenIndex=SESSION_PICKER | states 4,0x10 | Missing | — | collapsed with player-name state |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Menu header label | per-state QFOR header strip (screen title surf) | g_currentScreenIndex (CreateMenuStringLabelSurface(5)) | Implemented | frontend_init_return_screen path | rendered generically |
| Name input field renderer | RenderFrontendCreateSessionNameInput (states 2,6,0x12): live text + blinking caret | g_postRaceNameEditTargetPtr buffer | Partial | :7057 frontend_render_text_input() | single input, default text "New Session" prefill (orig prefills from g_localComputerName when empty, @0x41b0c0) |
| Sprite slot 0/1 | two animated decoration sprites slide in/out (states 1,3,5,7,0x11,0x13) | g_frontendAnimFrameCounter | Partial | timed-animation only (no positioned sprites) | port has no per-sprite slide; uses generic frontend_update_timed_animation |
| DXPlay join (state 0x14/0x15) | JoinSession + SNK_SeshJoinMsg broadcast; on fail → SESSION_LOCKED | dpu_exref session table | Missing (ARCH-DIVERGENCE) | :7081 collapsed | DXPTYPE wire incompatible; intentional |

PARITY VERDICT: Partial — flow shape (name entry → lobby/back) preserved; the second player-name stage and DXPlay handshake are intentionally collapsed (ARCH-DIVERGENCE: DXPTYPE).
GAPS (actionable):
- Player-name entry state (orig states 4/0x10, SNK_EnterPlayerNameButTxt) not reproduced; port goes name→lobby in one input.
- Button labels are literals ("Enter Name"/"Back"), not resolved SNK_EnterNewSessionNameButTxt / SNK_BackButTxt.
- Empty-name fallback to g_localComputerName (orig @0x41b0c0) absent; port hardcodes "New Session".
Confidence: [CONFIRMED @ 0x0041a7b0 decomp; port td5_frontend.c:7034-7090]. ARCH-DIVERGENCE annotated in both orig comment header and port :7074.

---

### Screen 11 @ 0x0041c330 — RunFrontendNetworkLobby  [interactive: Y]
Port handler: td5_frontend.c:7105 (Screen_NetworkLobby, case TD5_SCREEN_NETWORK_LOBBY)
DISPATCH MODEL: index-switch @0x0041c330 — `switch(g_frontendInnerState)` (18-state lobby); button clicks read `g_frontendButtonIndex` (1=ChangeCar→3, 4=Start, 5=Exit) in state 3.

BUTTONS (original order, created state 0):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | (NULL) decoration strip | 0 | non-interactive chat-input bar background (-0x1d0 wide, h=0x18) | always | Implemented | :7130 frontend_create_button("",...) | faithful (empty label) |
| 1 | SNK_MessageWindowButTxt ("MESSAGE WINDOW") | 0 | chat/message window surface (-0x200, h=0x80); non-click backing | always | Implemented | :7131 "Messages" | label literal not SNK |
| 2 | SNK_StatusButTxt ("STATUS") | 0 | player-roster panel surface (-0xe0, h=0x86); non-click backing | always | Implemented | :7132 "Status" | label literal not SNK |
| 3 | SNK_ChangeCarButTxt ("CHANGE CAR") | 0 | state3: g_frontendButtonIndex 2→3, pressed→lobbyPlayerStatus=1, SetFrontendScreen(CAR_SELECTION) | state 3 input | Partial | :7133 "Change Car"; lobby state3 handler ~:7200+ | DXPTYPE-gated; reachable but net incomplete |
| 4 | SNK_StartButTxt ("START") | 0 | idx4 pressed → lobbyPlayerStatus=2; host (slot==host) → state5 seal flow, else SNK_WaitForHostMsg broadcast | state 3, host/peer branch | Partial (ARCH-DIVERGENCE) | :7134 "Start" | seal/handshake collapsed |
| 5 | SNK_ExitButTxt ("EXIT") | 0 | idx5 pressed → lobbyPlayerStatus=1, g_returnToScreenIndex=2, slide-out state6 → SESSION_LOCKED/MAIN_MENU | state 3 | Implemented | :7135 "Exit" | faithful exit path |
| — | SNK_OkButTxt ("OK") | 0 | error-dialog OK (state 7, error branch, EXPLICIT x/y) | state 7, g_returnToScreenIndex∉{0,2} | Partial | error-dialog path | dialog ported structurally |
| — | SNK_YesButTxt / SNK_NoxButTxt ("YES"/"NO") | 0 | error-dialog confirm (state 7 yes/no branch, EXPLICIT x/y) | state 7, g_returnToScreenIndex∈{0,2} | Partial | error-dialog path | structural only |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Session header strip | QFOR g_lobbySessionHeaderSurface (480x32) under title | g_lobbySessionHeaderSurface | Partial | lobby render | structural |
| Chat panel | RenderFrontendLobbyChatPanel (state 3) | g_lobbyMsgRing* | Partial | lobby chat render | DXPTYPE: no live peers |
| Status/roster panel | RenderFrontendLobbyStatusPanel; per-slot status (0..3) | g_lobbySlotStatusTable[6] | Partial | lobby status render | shows local slot only |
| Chat input | RenderFrontendLobbyChatInput; 60-char (0x3c) buffer g_chatTokenizerCharClass | g_postRaceNameEditState | Partial | :7138 s_chat_input_buffer | input present, net send collapsed |
| Error dialog | g_lobbyErrorDialogSurface (482x64); SNK_NetErrString1-4 (idx*0x28/*0x20) centered | g_returnToScreenIndex (err code) | Partial | error-dialog states | strings not confirmed wired to SNK table |

PARITY VERDICT: Partial — full 18-state FSM mirrored for structure; all DirectPlay paths (seal, config broadcast, DXPSTART rendezvous, peer roster) are non-functional by design (ARCH-DIVERGENCE: DXPTYPE).
GAPS (actionable):
- Button labels are literals, not SNK_* (Messages/Status/Change Car/Start/Exit vs MESSAGE WINDOW/STATUS/CHANGE CAR/START/EXIT).
- Error-dialog text (SNK_NetErrString1-4) wiring to port surface not verified.
- Multi-peer roster, seal/handshake, error Yes/No semantics structural-only (DXPTYPE).
Confidence: [CONFIRMED @ 0x0041c330 decomp; port :7105-7448]. [UNCERTAIN: exact port line of state-3 button switch and error-string wiring — handler body spans :7160-7445, not line-mapped per-button.]

---

### Screen 12 @ 0x0041d890 — ScreenOptionsHub  [interactive: Y]
Port handler: td5_frontend.c:7456 (Screen_OptionsHub, case TD5_SCREEN_OPTIONS_HUB)
DISPATCH MODEL: index-switch @0x0041d8bc — state 6 `switch(g_frontendButtonIndex)` (0..5), gated by g_frontendButtonPressedFlag. Pure navigation menu (NO option toggles on this screen).

BUTTONS (original order):
| # | label (SNK_/literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_GameOptionsButTxt ("GAME OPTIONS") | 0 | g_returnToScreenIndex=GAME_OPTIONS(0xd), blit, slide out | pressed | Implemented | :7496 case 0 → TD5_SCREEN_GAME_OPTIONS | faithful |
| 1 | SNK_ControlOptionsButTxt ("CONTROL OPTIONS") | 0 | g_returnToScreenIndex=CONTROL_OPTIONS(0xe) | pressed | Implemented | :7497 case 1 → CONTROL_OPTIONS | faithful |
| 2 | SNK_SoundOptionsButTxt ("SOUND OPTIONS") | 0 | g_returnToScreenIndex=SOUND_OPTIONS; blit-secondary-to-primary | pressed | Implemented | :7498 case 2 → SOUND_OPTIONS | faithful |
| 3 | SNK_GraphicsOptionsButTxt ("GRAPHICS OPTIONS") | 0 | g_returnToScreenIndex=DISPLAY_OPTIONS | pressed | Implemented | :7499 case 3 → DISPLAY_OPTIONS | faithful |
| 4 | SNK_TwoPlayerOptionsButTxt ("TWO PLAYER OPTIONS") | 0 | g_returnToScreenIndex=TWO_PLAYER_OPTIONS | pressed | Implemented | :7500 case 4 → TWO_PLAYER_OPTIONS | faithful |
| 5 | SNK_OkButTxt ("OK") | 0 | **COMMITS shadow option globals → live**: g_cameraMode=g3dCollisionsConfigShadow^1; pool[0x1f8].velocity_z=gDynamicsConfigShadow; copies g_specialEncounterUnlockB→pool[0x1f0] bytes; gRaceDifficultyTier=g_raceDifficultyTier; _g_specialEncounterType=g_specialEncounterUnlockA; returnToScreen=MAIN_MENU | pressed | **Wrong/Missing** | :7501 case 5 → just s_return_screen=MAIN_MENU | port OK does NOT commit the game-options shadow values to the live race globals; just navigates back. Orig OK on the HUB is the commit point for difficulty/dynamics/3dcollisions/special-encounter shadows. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Menu header label | screen title (CreateMenuStringLabelSurface(6)) + SetFrontendInlineStringTable(SNK_Options_MT) | g_currentScreenIndex | Implemented | header path | faithful |
| 6 sliding button sprites | slide-in (state3) / slide-out (state8) anim | g_frontendAnimFrameCounter | Partial | timed-animation | generic timed anim, not per-sprite path |

PARITY VERDICT: Divergent (one mechanism) — navigation faithful, but the OK-button shadow→live commit (the screen's only side-effecting action) is absent in the port.
GAPS (actionable):
- **OK button (idx 5) must commit option shadows**: orig @0x41dabe sets g_cameraMode (=^1 of 3dCollisions shadow), dynamics→pool velocity_z, special-encounter A/B, gRaceDifficultyTier←g_raceDifficultyTier. Port only sets s_return_screen=MAIN_MENU. Verify whether the port commits these elsewhere (e.g. on race start) — if so it's a deferred-commit divergence, else a functional gap.
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041d890 decomp; port :7456-7525]. [UNCERTAIN: whether port commits difficulty/dynamics/3dcollisions shadows at race start instead — MEMORY notes difficulty_tier routing exists at td5_physics/ai; need cross-check of g_td5.difficulty_tier write path vs this OK handler.]

---

### Screen 13 @ 0x0041f990 — ScreenGameOptions  [interactive: Y]
Port handler: td5_frontend.c:7534 (Screen_GameOptions, case TD5_SCREEN_GAME_OPTIONS)
DISPATCH MODEL: index-switch @0x0041ffef — state 6: arrow `switch(g_frontendButtonIndex)` (0..6) gated by g_postRaceRacerCardNavDirection (=±1); OK = idx7 gated by g_frontendButtonPressedFlag. Each row = OPTION CYCLER writing a config shadow global.

BUTTONS / OPTION ROWS (original order):
| # | label (SNK_) | userdata | option global written + value set | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_CircuitLapsTxt ("CIRCUIT LAPS") | 0 | gCircuitLapsConfigShadow += dir; clamp [0,3] | arrow | Partial | :7581 s_game_option_laps +=delta, wrap 0..3 | port WRAPS (0→3, 3→0); **orig CLAMPS** (stays at 0 or 3). Behavior diverges at ends. |
| 1 | SNK_CheckpointTimersButTxt ("CHECKPOINT TIMERS") | 0 | g_specialEncounterUnlockA_PROVISIONAL = (dir+val)&1 (0/1 toggle) | arrow | Implemented | :7586 s_game_option_checkpoint_timers ^=1 | faithful toggle |
| 2 | SNK_TrafficButTxt ("TRAFFIC") | 0 | g_specialEncounterUnlockB_PROVISIONAL = (dir+val)&1 | arrow | Implemented | :7589 s_game_option_traffic ^=1 | faithful |
| 3 | SNK_CopsButTxt ("POLICE") | 0 | gSpecialEncounterConfigShadow = (dir+val)&1 | arrow | Implemented | :7592 s_game_option_cops ^=1 | faithful (label "Police") |
| 4 | SNK_DifficultyButTxt ("DIFFICULTY") | 0 | g_raceDifficultyTier += dir; if <0→2, if >2→0 (3-way WRAP) | arrow | Implemented | :7595 wrap 0..2 (−→2, >2→0) | faithful wrap |
| 5 | SNK_DynamicsButTxt ("DYNAMICS") | 0 | gDynamicsConfigShadow = (dir+val)&1 (0=SIMULATION,1=ARCADE) | arrow | Implemented | :7600 s_game_option_dynamics ^=1 | faithful |
| 6 | SNK_3dCollisionsButTxt ("3D COLLISIONS") | 0 | g3dCollisionsConfigShadow = (dir+val)&1 | arrow | Implemented | :7603 s_game_option_collisions ^=1 | faithful |
| 7 | SNK_OkButTxt ("OK") | 0 | g_returnToScreenIndex=OPTIONS_HUB(0xc); slide out | pressed | Implemented | :7607 idx7 → OPTIONS_HUB | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Value panel (224x280) | g_lobbyErrorDialogSurface; 7 value strings drawn via MeasureOrCenter+DFLS each redraw (state 4) | per-row shadow globals | Implemented | :3972 frontend_render_game_options_overlay | faithful value text |
| Row 0 value | sprintf(g_uiFormatStringScratchTemplate) numeric laps | gCircuitLapsConfigShadow | Partial | :3979 "%d",(laps+1)*2 | **value MAPPING differs**: orig laps row uses sprintf template; port shows (laps+1)*2 (= 2,4,6,8). [UNCERTAIN orig literal lap numbers] |
| Rows 1-3,6 value | SNK_OnOffTxt[val] ("OFF"/"ON") | each toggle shadow | Implemented | :3981-3986 on_off[] | faithful |
| Row 4 value | SNK_DifficultyTxt[tier] (EASY/NORMAL/HARD) | g_raceDifficultyTier | Implemented | :3984 difficulty[] | faithful |
| Row 5 value | SNK_DynamicsTxt[val] (SIMULATION/ARCADE) | gDynamicsConfigShadow | Implemented | :3985 dynamics[] | faithful |
| Per-row ◄► arrows | InitializeFrontendDisplayModeArrows(0..6,1) — all 7 rows get arrows | — | Implemented | frontend arrow render :3972area | faithful (7 rows arrowed) |

PARITY VERDICT: Faithful (one divergence) — option rows, globals, value text, arrows all match; only Circuit Laps end-behavior differs (port wraps, orig clamps).
GAPS (actionable):
- **Circuit Laps: orig CLAMPS at [0,3]** (no wrap); port wraps 3→0 / 0→3 (:7582-7583). Fix to clamp.
- Confirm laps display mapping `(laps+1)*2` vs orig sprintf template literal numbers.
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041f990 decomp + port :7534/3972]. Orig case-6 clamp/wrap rules read verbatim from decomp.

---

### Screen 14 @ 0x0041df20 — ScreenControlOptions  [interactive: Y]
Port handler: td5_frontend.c:7636 (Screen_ControlOptions, case TD5_SCREEN_CONTROL_OPTIONS)
DISPATCH MODEL: index-switch @0x0041e5xx — state 6: arrow branch on g_frontendButtonIndex (0=P1 device cycle, 2=P2 device cycle) gated by g_postRaceRacerCardNavDirection; pressed branch (1=Config P1, 3=Config P2, 4=OK). Rows 0/2 are DEVICE-SOURCE CYCLERS (not navigation).

BUTTONS (original order):
| # | label (SNK_) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_Player1ButTxt ("PLAYER 1") | 0 | **arrow → cycle g_player1InputSource** (+dir &7, skip empty devices via g_player1DeviceDesc[], skip == player2 source); redraw state4 | arrow | **Missing** | :7644 frontend_create_preview_button("Player 1") | port treats row 0 as inert preview button; NO device-source cycling on arrows. Orig changes which input device P1 uses. |
| 1 | SNK_ConfigureButTxt ("CONFIGURE") | 0 | pressed → g_controllerBindingActivePlayerSlot=0, g_returnToScreenIndex=CONTROLLER_BINDING(0x12) | pressed | Implemented | :7674 idx1/3 → CONTROLLER_BINDING, s_ctrl_player=0 | faithful |
| 2 | SNK_Player2ButTxt ("PLAYER 2") | 0 | **arrow → cycle g_player2InputSource** (+dir &7, skip empty, skip == P1) | arrow | **Missing** | :7646 frontend_create_preview_button("Player 2") | same gap as row 0 for P2 |
| 3 | SNK_ConfigureButTxt ("CONFIGURE") | 0 | pressed → g_controllerBindingActivePlayerSlot=1, CONTROLLER_BINDING | pressed | Implemented | :7676 s_ctrl_player=(idx==3)?1:0 | faithful |
| 4 | SNK_OkButTxt ("OK") | 0 | pressed → g_returnToScreenIndex=OPTIONS_HUB (or self if from binding return); slide out | pressed | Implemented | :7680 idx4 → OPTIONS_HUB | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| P1 device icon | QFOR g_soundOptionsMenuVolume (Controllers.tga) src_y=local_84<<5, 64x32, at (cx+0x4a, cy-0x8f); local_84 derived from g_player1DeviceDesc[g_player1InputSource] (0=kbd,1=joy,2=400,3=600,4=none→hidden) | g_player1InputSource | Implemented | :4511 fe_draw_quad 394,97 row=p1_type*32 | port uses td5_input_get_device_type(0) for row; faithful icon |
| P2 device icon | same, src_y=local_88<<5, at (cx+0x4a, cy-0x17 = 217) | g_player2InputSource | Implemented | :4513 fe_draw_quad 394,217 | faithful icon |
| Device name value panel (224x160) | g_lobbyErrorDialogSurface; state4 draws P1 then P2 device-name string via MeasureOrCenter(x=0x4a)+DFLS: keyboard→"%s" (DAT_004658e4), joy/pad→"%s %d" (s__s__d_0046603c, name+index) | g_player1/2InputSource | **Missing** | — | port renders icons only; the device NAME text strings ("%s"/"%s %d") drawn into the value panel are NOT reproduced. Panel exists but no device-name text. |
| Row 0/2 ◄► arrows | InitializeFrontendDisplayModeArrows(0,1) and (2,1) only (rows 1,3,4 have no arrows) | — | **Missing** | — | port renders no selector arrows on Player1/Player2 rows AND has no cycle handler → device source cannot be changed in port |
| Header / inline ex-string | SetFrontendInlineStringEntry(9,SNK_CtrlOptions_Ex) when returning from binding | g_returnToScreenIndex==~LOCALIZATION_INIT | Partial | header path | extra hint line not confirmed |

PARITY VERDICT: Divergent — Configure/OK navigation faithful, but the screen's PRIMARY interactive feature (cycling each player's input device via ◄► on the Player1/Player2 rows) is entirely missing, along with the device-name text labels. Port can only enter the binding sub-screen; it cannot change device assignment here.
GAPS (actionable):
- **Implement P1/P2 device cycling** (orig state6 idx0/idx2): on arrow, advance g_playerNInputSource (+dir &7), skip slots where g_playerNDeviceDesc[idx]==0, skip when equal to the other player's source. Port currently has no `frontend_option_delta()` call in Control Options case 6.
- **Render device-name strings** into the value panel: keyboard → "%s" device name; joystick/joypad → "%s %d" (name + 1-based index). Port draws only the icon row.
- **Add ◄► arrows** on rows 0 and 2 (orig InitializeFrontendDisplayModeArrows(0,1)/(2,1)).
- Button labels literal, not SNK_*.
Confidence: [CONFIRMED @ 0x0041df20 decomp; device strings DAT_004658e4="%s", s__s__d_0046603c="%s %d" read from memory; port :4489 overlay + :7636 handler]. local_84/local_88 device-index = runtime (g_player1DeviceDesc table), confirmed in decomp.

---

## Cross-screen summary
- Dispatch: all 5 = index-switch on g_frontendInnerState + g_frontendButtonIndex; userdata always literal 0 (no callback/userdata model). Port mirrors with s_inner_state + s_button_index. CONFIRMED.
- Recurring port gap (cosmetic): all button labels are English literals, not the resolved SNK_*ButTxt LANGUAGE.DLL strings (non-functional but a localization fidelity gap).
- Functional Missing/Wrong:
  - S10: player-name 2nd-stage + empty-name→computer-name fallback Missing (DXPTYPE-adjacent).
  - S12: OK-button shadow→live commit Missing/Wrong (only side-effect of the hub).
  - S13: Circuit Laps clamp-vs-wrap Wrong.
  - S14: device-source cycling Missing; device-name text Missing; row 0/2 arrows Missing.
# Frontend BEHAVIOR Harvest — Part 15 (screen-table indices 15–19)

Source: `TD5_d3d.exe`. Functions decompiled in full (read_only Ghidra session, pool slot TD5_pool10).
Port handlers: `td5mod/src/td5re/td5_frontend.c`.

## Shared dispatch model (all 5 screens)

These are NOT button-list screens with per-button userdata. Every `CreateFrontendDisplayModeButton`
is called with `userdata = 0` (always; verified in all 5 decomps). The dispatch is an
**index-switch on `g_frontendButtonIndex`** (current selected row), combined with two driver globals:

- `g_frontendButtonIndex` — selected row 0..N (set by the up/down cursor nav, shared `RunFrontendDisplayLoop`).
- `g_postRaceRacerCardNavDirection` — LEFT/RIGHT arrow delta (-1 / +1 / 0). When != 0 → a **cycler** action.
- `g_frontendButtonPressedFlag` — confirm/press edge. When != 0 → a **commit** action (Music Test / OK).
- `g_frontendEscKeyButtonIndex` — which row index ESC maps to (always the OK row).

Action handling lives ONLY in case 6 (interactive state) for the options screens (15/16/17/19),
and in cases 0xa / 0x1a (live capture) for the binding screen (18). The states 0/1/2/3/4/5/7/8 are
init / present / slide-in / static-redraw / prep-slideout / slide-out animation. The userdata=0 means
the button's action is implied purely by its ordinal index in case 6 — confirmed identical model in port
(`s_button_index` switch, no userdata field).

Confirmed helper semantics:
- `CreateFrontendDisplayModeButton @0x425de0(label,x,y,w,h,ud)` — ud always 0.
- `InitializeFrontendDisplayModeArrows(rowIdx,1)` — marks a row as a `◄►` cycler (only cycler rows get this call).
- `g_frontendEscKeyButtonIndex = N` — maps ESC to the OK row.

---

### Screen 15 @ 0x0041ea90 — ScreenSoundOptions  [interactive: Y]
Port handler: td5_frontend.c:7706 (case TD5_SCREEN_SOUND_OPTIONS, dispatch td5_frontend.c:5530 render overlay)
DISPATCH MODEL: index-switch @0x0041f2d0 (case 6) — switch(g_frontendButtonIndex); rows 0/1/2 are cyclers (nav!=0), rows 3/4 are commits (press).

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_SfxModeButTxt ("SFX Mode") | 0 | nav!=0: cycle g_sfxPlaybackMode; if CanDo3D()→3-mode(0,1,2) else 2-mode(0,1); DXSound::SetPlayback(); inner_state=4 | InitializeFrontendDisplayModeArrows(0,1) | Implemented | :7743-7751 | port cycles 0..2 UNCONDITIONALLY (drops CanDo3D 2-mode fallback; documented ARCH-DIV, faithful for D3D11/DSound) |
| 1 | SNK_SfxVolumeButTxt ("SFX Volume") | 0 | nav!=0: g_persistedMasterVolumePercent += dir*10, clamp 0..100; DXSound::SetVolume((pct*0xfc00)/100 & 0xfc00); inner_state=4 | InitializeFrontendDisplayModeArrows(1,1) | Implemented | :7752-7759 | step dir*10 matches; persists via td5_save_set_sfx_volume + td5_sound_set_sfx_volume |
| 2 | SNK_MusicVolumeButTxt ("Music Volume") | 0 | nav!=0: g_persistedCdVolumePercent += dir*10, clamp 0..100; DXSound::CDSetVolume((pct*0xfc00)/100 & 0xfc00); inner_state=4 | InitializeFrontendDisplayModeArrows(2,1) | Implemented | :7760-7766 | orig adjusts CD volume (CDSetVolume); port labels it "music_volume" + td5_sound_set_music_volume — semantic match (this IS CD/music) |
| 3 | SNK_MusicTestButTxt ("Music Test") | 0 | press: g_returnToScreenIndex=TD5_SCREEN_MUSIC_TEST; slide-out → screen 19 | none (not a cycler) | Implemented | :7770-7772 | faithful |
| 4 | SNK_OkButTxt ("OK") | 0 | press: g_returnToScreenIndex=TD5_SCREEN_OPTIONS_HUB; slide-out | g_frontendEscKeyButtonIndex=4 | Implemented | :7773-7775 | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| SFX-mode icon | Controllers.tga blit, src_y=(g_sfxPlaybackMode+4)*0x20 → 3 distinct rows (mono/stereo/surround) | g_sfxPlaybackMode | Wrong | :4021-4029 | port uses separate Stereo.tga/Mono.tga selected by `(mode & 1)` → only 2 visual states; 3rd mode (surround, mode==2) shows the Mono icon. Loads Stereo.tga/Mono.tga not Controllers.tga |
| SFX volume bar | VolumeBox bg (0xe0×0xc) + VolumeFill (≤0xde) | g_persistedMasterVolumePercent | Implemented | :4035-4061 | bar+fill rendered; fill width vol/100*222 matches orig (pct*0xde/100) |
| Music/CD volume bar | VolumeBox bg + VolumeFill | g_persistedCdVolumePercent | Implemented | :4035-4061 | second bar rendered |
| header label | CreateMenuStringLabelSurface(6), slide-animated | g_currentScreenIndex | Implemented | (shared header path) | title surface |
| value box | g_lobbyErrorDialogSurface 0xe0×0xa0 holds centered SNK_SFX_Modes[mode] text | g_lobbyErrorDialogSurface | Partial | :4010 | port draws NO text label for the mode (comment "no extra text needed"); orig renders SNK_SFX_Modes[mode] string into the box |

PARITY VERDICT: Partial — actions/cyclers/commits all faithful; SFX-mode icon uses 2-state Mono/Stereo instead of orig 3-row Controllers.tga, and the mode TEXT label is omitted.
GAPS (actionable):
- SFX-mode icon: replace 2-state Stereo/Mono `& 1` (td5_frontend.c:4023) with Controllers.tga blit src_y=(mode+4)*0x20 so the 3rd "surround" mode is visually distinct.
- Render the SNK_SFX_Modes[mode] text label into the value box (orig case-4 path, td5_frontend.c:4010 omits it).
Confidence: [CONFIRMED @ 0x0041ea90 full decomp; case-6 dispatch @0x0041f2d0; SetVolume/CDSetVolume/SetPlayback confirmed]. [UNCERTAIN: literal text of SNK_SFX_Modes[] — lives in LANGUAGE.DLL, not this binary; not resolvable in slice].

---

### Screen 16 @ 0x00420400 — ScreenDisplayOptions  [interactive: Y]
Port handler: td5_frontend.c:7799 (case TD5_SCREEN_DISPLAY_OPTIONS)
DISPATCH MODEL: index-switch @0x004208fa (case 6) — switch(g_frontendButtonIndex) {0..3 cyclers, 4=OK}.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_ResolutionButTxt ("Resolution") | 0 | nav!=0: gConfiguredDisplayModeOrdinal += dir; wrap on empty g_displayModeStringTable entry (back→last, fwd→0); inner_state=4 | InitializeFrontendDisplayModeArrows(0,1) | Implemented | :7843-7855 | port also calls td5_plat_apply_display_mode() + td5_save_set_display_mode() (orig only sets ordinal here; applied elsewhere) — additive but correct |
| 1 | SNK_FoggingButTxt ("Fogging") | 0 | nav!=0: gFoggingConfigShadow = (dir+val)&1; inner_state=4 | created via CreateFrontendDisplayModeButton + Arrows(1,1) ONLY if DXD3D::CanFog()==1; else CreateFrontendDisplayModePreviewButton (disabled, no arrows) | Partial | :7856-7859 | port ALWAYS creates a normal cycler button (no CanFog() gate) → fog row is always interactive; orig disables it (preview button, no toggle) on no-fog hardware |
| 2 | SNK_SpeedReadoutButTxt ("Speed Readout") | 0 | nav!=0: gSpeedReadoutUnitsConfigShadow = (dir+val)&1; inner_state=4 | InitializeFrontendDisplayModeArrows(2,1) | Implemented | :7860-7862 | s_display_speed_units = !s_display_speed_units; matches &1 toggle |
| 3 | SNK_CameraDampingButTxt ("Camera Damping") | 0 | nav!=0: g_cameraSpeedSetting += dir, clamp 0..9; inner_state=4 | InitializeFrontendDisplayModeArrows(3,1) | Implemented | :7863-7867 | clamp 0..9 matches |
| 4 | SNK_OkButTxt ("OK") | 0 | press: g_returnToScreenIndex=TD5_SCREEN_OPTIONS_HUB; slide-out | g_frontendEscKeyButtonIndex=4 | Implemented | :7868-7870 | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| values box | g_lobbyErrorDialogSurface 0xe0×0x118; in case 4 redraws 4 centered labels: resolution string, On/Off fog (only if CanFog), speed-unit string, camera-damping number (sprintf) | g_lobbyErrorDialogSurface | Implemented | frontend_refresh_display_option_labels :7816,7831 | port refreshes labels in case 0 + case 4; live-rendered. Fog label always shown (no CanFog gate) |
| preview/disabled fog button | CreateFrontendDisplayModePreviewButton variant (grayed, non-cycling) when DXD3D::CanFog()!=1 | (DXD3D::CanFog) | Missing | n/a | port has no preview/disabled-button variant; fog is always a live button |
| header label | CreateMenuStringLabelSurface(6) | g_currentScreenIndex | Implemented | shared | |

PARITY VERDICT: Partial — all 5 rows + dispatch faithful; only divergence is the CanFog()-gated fog DISABLE (orig renders a non-interactive preview button on no-fog hardware; port always allows fog toggle).
GAPS (actionable):
- Gate the Fogging row on a CanFog()-equivalent: when fog unsupported, create a disabled/preview button (no arrows) instead of a live cycler (td5_frontend.c:7812 + 7856). Low impact on D3D11 (fog generally supported).
Confidence: [CONFIRMED @ 0x00420400 full decomp; case-6 switch @0x004208fa; CanFog branch @0x00420484]. [UNCERTAIN: CreateFrontendDisplayModePreviewButton entry addr not resolved — name-only].

---

### Screen 17 @ 0x00420c70 — ScreenTwoPlayerOptions  [interactive: Y]
Port handler: td5_frontend.c:7895 (case TD5_SCREEN_TWO_PLAYER_OPTIONS)
DISPATCH MODEL: index-switch @0x00421040 (case 6) — switch(g_frontendButtonIndex) {0=SplitScreen cycler, 1=Catchup cycler, 2=OK}.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_SplitScreenButTxt ("Split Screen") | 0 | nav!=0: g_twoPlayerSplitMode = (dir+val)&1; inner_state=4 | InitializeFrontendDisplayModeArrows(0,1) | Implemented | :7932-7941 | (dir+val)&1 byte-matches; port also syncs s_two_player_mode bit2 (additive) |
| 1 | SNK_CatchupTxt ("CATCHUP") | 0 | nav!=0: g_twoPlayerCatchupAssist += dir, clamp 0..9; inner_state=4 | InitializeFrontendDisplayModeArrows(1,1) | Implemented | :7942-7948 | clamp 0..9 matches |
| 2 | SNK_OkButTxt ("OK") | 0 | press: g_returnToScreenIndex=TD5_SCREEN_OPTIONS_HUB; slide-out | g_frontendEscKeyButtonIndex=2 | Implemented | :7949-7951 | faithful |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| split-mode icon | SplitScreen.tga blit, src_y=g_twoPlayerSplitMode<<5 (2 rows) at x=394 y=97 | g_twoPlayerSplitMode | Implemented | frontend_render_two_player_options_overlay :4139-4155 | icon rendered, v-row = mode<<5 equivalent, CONFIRMED-annotated |
| values box | g_lobbyErrorDialogSurface 0xe0×0x78; case 4 draws SNK_Split_Modes[mode] + catchup number (sprintf) | g_lobbyErrorDialogSurface | Partial | :4127-4134 | port draws "ON"/"OFF" text for split + (catchup!=0?ON:OFF) instead of orig SNK_Split_Modes[mode] string + the numeric catchup LEVEL (0..9). Catchup shown as binary ON/OFF, loses the level number |
| header label | CreateMenuStringLabelSurface(6) | g_currentScreenIndex | Implemented | shared | |

PARITY VERDICT: Faithful (minor label divergence) — buttons + cyclers + commit byte-matched; split-mode icon rendered; only the value-box labels differ (port shows ON/OFF text vs orig SNK_Split_Modes[mode] string + numeric catchup level).
GAPS (actionable):
- Catchup value box: orig shows the catchup LEVEL number 0..9 (sprintf, case 4 @0x420E80); port shows binary "ON"/"OFF" (td5_frontend.c:4134) — restore the numeric level. Split-mode label could use the SNK_Split_Modes string instead of "ON"/"OFF".
Confidence: [CONFIRMED @ 0x00420c70 full decomp; case-6 @0x00421040; splitmode<<5 icon + catchup clamp 0..9; port overlay rendered @ td5_frontend.c:4126 dispatch :5540].

---

### Screen 18 @ 0x0040fe00 — ScreenControllerBindingPage  [interactive: Y]
Port handler: td5_frontend.c:8009 (case TD5_SCREEN_CONTROLLER_BINDING)
DISPATCH MODEL: callback-via-device-branch + live capture state machine @0x0040fe00 — TWO parallel flows selected at case 0 by device type; the "buttons" here are NOT a menu list, they are per-action binding ROWS captured live.

State machine (orig → port):
- case 0 init: resolve device for g_controllerBindingActivePlayerSlot (P1=g_player1InputSource / P2=g_player2InputSource). If device type byte==3 (none/keyboard) → state 0x13 (kbd). Wheel (desc&0xff00==0x600) → DrawControlBindingTextWithOkButton delegate. Joystick → set g_controllerBindingButtonCount (<4→2, ==2 special, >=9→8), set active flag [player*9]=1, validate axis slots (must be 4|5 else reset 4,5) → state 9.
- JOYSTICK flow: 9 slide-in → 0xa (live) → 0xb slide-out.
- KEYBOARD flow: 0x13 slide-in → 0x14 init (clears 4 scancode DWORDs g_keyboardScanCodeTable/+58/+5c/scrollOffset, slot=0) → 0x19 (show action label SNK_ControlText[slot*0x10]) → 0x1a (capture) → repeat to slot==10 → 0xb/0x1b slide-out.

CAPTURE STATE MACHINE (the core behavior):
- JOYSTICK (case 0xa): each frame shift-register reads DXInput::GetJS(deviceIdx-1):
  `prev=held; held=~curr; curr=GetJS(); held&=curr` (rising edge). For each slot i<count, bit=0x40000<<i:
  if (prev&bit)&&(curr&bit) → binding_table[player*9+i]++ ; if >10 → wrap to 2 (cycles 2..10).
  Special count==2: button 0x40000 or 0x80000 held → SWAP steer/throttle axis slots [player*9] and cache.
  OK (g_frontendButtonIndex==0 + press) → state 0xb.
- KEYBOARD (case 0x1a): scan scancodes 0..0xff; skip codes already in g_keyboardScanCodeTable buffer;
  DXInput::CheckKey(code)!=0 → write `*(byte*)(&g_keyboardScanCodeTable + progressIndex) = code`; DXSound::Play(3);
  progressIndex++; if !=10 → state 0x19 (next action) else → OK/slide-out 0x1b.

Binding tables written (orig):
- joystick: g_controllerBindings / g_controllerBindingsCache_PROVISIONAL / g_controllerBindings_current_PROVISIONAL, row stride 9, [player*9+slot].
- keyboard: g_keyboardScanCodeTable (DAT_00464054), 10 bytes per player.

"BUTTONS" / capture rows (action order = SNK_ControlText @ LANGUAGE.DLL 0x100075E0 + slot*0x10):
| slot | label (confirmed prior RE) | userdata | action | PORT status | port ref | gap |
|---|---|---|---|---|---|---|
| 0 | LEFT | n/a | kbd: capture scancode→g_keyboardScanCodeTable[0]; joy: cycle slot value | Implemented | :8279-8363 | k_ctrl_action_labels confirmed correct (prior 2026-05-30 fix) |
| 1 | RIGHT | n/a | capture/cycle | Implemented | :8300-8363 | |
| 2 | ACCELERATE | n/a | capture/cycle | Implemented | :8300-8363 | |
| 3 | BRAKE | n/a | capture/cycle | Implemented | :8300-8363 | |
| 4 | HANDBRAKE | n/a | capture/cycle | Implemented | :8300-8363 | |
| 5 | HORN/SIREN | n/a | capture/cycle | Implemented | :8300-8363 | |
| 6 | GEAR UP | n/a | capture/cycle | Implemented | :8300-8363 | |
| 7 | GEAR DOWN | n/a | capture/cycle | Implemented | :8300-8363 | |
| 8 | CHANGE VIEW | n/a | capture/cycle | Implemented | :8300-8363 | |
| 9 | REAR VIEW | n/a | capture/cycle | Implemented | :8300-8363 | |
| (OK btn) | SNK_OkButTxt | 0 | press g_frontendButtonIndex==0 → slide-out → TD5_SCREEN_CONTROL_OPTIONS | Implemented | :8216-8229 | only real CreateFrontendDisplayModeButton on this screen |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| controller-type icon | per-type TGA centered y=120 (keyboard/joypad/joystick) | td5_input_get_device_type | Implemented | frontend_render_controller_binding_overlay :4527-4545 | icon rendered per device type |
| per-button light row | ButtonLights.tga blit 0x10×0x10, src_y=0(off)/0x10(pressed) per joystick button | g_controllerBindingPage_inputCursor + g_controllerBindingCurrentButtons | Partial | :4556-4583 | port renders a BUTTON/ACTION text grid (BTN1..N + k_js_value_labels) instead of the ButtonLights.tga pressed-state light; no live pressed-light because joystick input is a KB-arrow surrogate |
| binding text box | g_lobbyErrorDialogSurface 0x1c0×0xd8, per-action labels | g_lobbyErrorDialogSurface | Implemented (as live text) | :4556-4583 | port draws action rows as live fe_draw_text, not an offscreen 0x1c0×0xd8 surface (functionally equivalent) |
| keyboard capture prompt | g_controllerBindingPage_state 0x1c0×0x40 "Press Key"/action label | g_controllerBindingPage_state | Implemented (as live text) | :4593-4597+ | port draws "PRESS KEY FOR: [ACTION]" live during states 20/25/26/27 |
| keyboard binding table | 10 scancodes per player | g_keyboardScanCodeTable | Implemented (bridged) | :8330,8345-8352 | port writes s_ctrl_kb_scancodes → s_p1/p2_custom_bindings + td5_plat_input_set_keyboard_bindings → s_kb_bindings[2][10] (td5_platform_win32.c:72,932). Applies live + persists Config.td5. CONFIRMED wired (matches prior 2026-05-30 rebind-applies fix) |
| joystick binding table | [player*9+slot] cycle values | g_controllerBindings* | Partial | :8222 | port copies s_ctrl_binding_table → td5_save_get_controller_bindings_mutable (td5_save.c:218) + Config.td5; but joystick INPUT is a KB-arrow surrogate, never reads real DXInput::GetJS |

PARITY VERDICT: Partial — keyboard capture loop + binding-table write/persist/apply faithful and CONFIRMED wired (s_kb_bindings bridge verified); device icon + action rows + capture prompt all rendered (as live text vs orig offscreen surfaces). Only real divergence: joystick path uses a keyboard-arrow surrogate instead of live DXInput::GetJS.
GAPS (actionable):
- Joystick capture: replace the KB-arrow surrogate (td5_frontend.c:8160-8178) with a real joystick bitmask source (td5_input_get_control_bits / DXInput::GetJS equivalent). On KB-only setups the screen routes to the keyboard flow anyway, so this only affects gamepad/wheel users.
- Joystick button-light pressed-state (ButtonLights.tga src_y 0/0x10) not shown live (port draws BTN/ACTION text grid instead, td5_frontend.c:4556-4583) — cosmetic.
- Wheel device path (desc&0xff00==0x600 → DrawControlBindingTextWithOkButton) has no port branch.
Confidence: [CONFIRMED @ 0x0040fe00 full decomp incl. both flows; rising-edge shift register; scancode scan loop; g_keyboardScanCodeTable write @0x00410xxx; binding tables stride-9]. Port KB-bridge CONFIRMED via td5_platform_win32.c:72/932 + td5_save.c:218-220. [UNCERTAIN: DrawControlBindingTextWithOkButton / OpenControllerBindingPageNoneHeader delegate entry addrs not resolved this pass].

---

### Screen 19 @ 0x00418460 — ScreenMusicTestExtras  [interactive: Y]
Port handler: td5_frontend.c:8391 (case TD5_SCREEN_MUSIC_TEST)
DISPATCH MODEL: index-switch @0x00418650 (case 6) — switch(g_frontendButtonIndex) {0=SelectTrack: cycle (nav!=0) OR play (press), 1=OK}.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_SelectTrackButTxt ("Select Track") | 0 | nav!=0: g_selectedCdTrackIndex += dir, WRAP 0↔0xb (back→0xb, fwd→0); redraw track-# box. press(nav==0): DXSound::CDPlay(idx+2,1); g_attractCdTrackCandidate=idx; redraw now-playing box (NowPlaying + band + title) | InitializeFrontendDisplayModeArrows(0,1) | Partial | :8442-8466 | port CLAMPS idx 0..11 (no wrap) — DEVIATION: orig WRAPS (0x418xxx: <0→0xb, >0xb→0). Port comment at :8448 wrongly says "clamps, does NOT wrap" but orig decomp wraps |
| 1 | SNK_OkButTxt ("OK") | 0 | press: g_returnToScreenIndex=TD5_SCREEN_SOUND_OPTIONS; g_extrasGalleryCrossFadePhase=0x40; slide-out | g_frontendEscKeyButtonIndex=1 | Implemented | :8467-8470 | faithful (returns to Sound Options) |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| track-# box | g_lobbyErrorDialogSurface 0x170×0x28; sprintf "%d. %s" (track number + band) | g_selectedCdTrackIndex | Implemented | frontend_music_test_update_track_label :4066,8416,8451 | "1. GRAVITY KILLS" style, live-rendered |
| now-playing box | g_musicTestSelectedTrackId 0x170×0x78; NowPlaying text + band (PTR_s_GRAVITY_KILLS[idx]) + title (PTR_s_FALLING[idx]) | g_musicTestSelectedTrackId | Implemented | frontend_music_test_update_now_playing :4075,8463 | band/title tables ported verbatim (td5_frontend.c:160-167) |
| band/title string tables | 12-entry pointer arrays @0x465e1c (band) / 0x465e58 (title) | g_selectedCdTrackIndex | Implemented | :157-167 | CONFIRMED matches binary data (band ptrs read at 0x465e1c) |
| cross-fade gate | case 0 init gated by g_extrasGalleryCrossFadePhase (<-0xf to init; >0xc0 mirror; >0x40 clamp) | g_extrasGalleryCrossFadePhase | Missing | n/a | port has no crossfade-phase gate; band-gallery image cross-fade not ported (no offscreen gallery surface pool) |
| header label | CreateMenuStringLabelSurface(6) | g_currentScreenIndex | Implemented | shared | |

PARITY VERDICT: Partial — both buttons + CDPlay + track/now-playing boxes faithful (band/title tables verbatim); divergences are the track-index WRAP (port clamps) and the missing gallery cross-fade phase.
GAPS (actionable):
- Track index should WRAP not clamp: orig (0x418460 case 6, nav!=0 branch) sets idx=0xb when <0 and idx=0 when >0xb; port clamps 0..11 (td5_frontend.c:8449-8450). Fix the comment at :8448 (it incorrectly states orig clamps) and restore wrap.
- Gallery cross-fade (g_extrasGalleryCrossFadePhase) not ported — band-photo fade-in on entry/exit absent (acknowledged ARCH-DIV: no gallery surface pool).
Confidence: [CONFIRMED @ 0x00418460 full decomp; CDPlay(idx+2,1) @case 6; wrap 0↔0xb; band ptr table @0x465e1c verified via memory_read]. [UNCERTAIN: literal text of SNK_SelectTrackButTxt etc — LANGUAGE.DLL].

---

## Cross-screen summary
- Dispatch is uniformly index-switch on `g_frontendButtonIndex` (port `s_button_index`/`s_selected_button`), NOT userdata. All button userdata == 0.
- LEFT/RIGHT cyclers gated by `g_postRaceRacerCardNavDirection != 0` (port `frontend_option_delta()`); confirm/OK gated by `g_frontendButtonPressedFlag` (port `s_input_ready` + `s_button_index`).
- Cycler rows are flagged by `InitializeFrontendDisplayModeArrows(rowIdx,1)`; OK row by `g_frontendEscKeyButtonIndex`.
- All option screens persist via DXSound::Set*/Config writes; port mirrors with td5_save_set_* + td5_sound_set_* + Config.td5.
- Confirmed Missing/Wrong tally: 2 Wrong (screen-15 SFX icon 2-state; screen-19 track-index clamp-not-wrap), 3 Missing (screen-16 fog-disable preview button, screen-18 wheel device path, screen-19 gallery crossfade). Plus several Partial label divergences (screen-15 mode text, screen-17 catchup level shown as ON/OFF, screen-18 joystick KB-arrow surrogate).
# Frontend Behavior + Port-Parity Harvest — Part 20 (table indices 20–24)

Source: `TD5_d3d.exe` (image base 0x00400000), Ghidra read-only session (reused open pool10 session for TD5_d3d.exe — slot lock contention on fresh acquire). Port = `td5mod/src/td5re/td5_frontend.c`.

DISPATCH-MODEL NOTE (all 5 screens): the original frontend has NO per-button userdata callback. `CreateFrontendDisplayModeButton @0x00425de0(label,x,y,w,h,ud)` is always called with `ud=0`; the screen FSM dispatches on `g_frontendButtonIndex` (the focused/clicked button, by CREATION ORDER) and `g_postRaceRacerCardNavDirection` (◄►/arrow nav = the global `DAT_0049b690`, ±1). The port mirrors this exactly: `frontend_create_button(label,x,y,w,h)` (NO ud param), and an `s_button_index` / `frontend_option_delta()` (reads `s_arrow_input` → ±1) switch. So "userdata" below = "N/A — index dispatch" for every button on both sides. `CreateFrontendMenuRectEntry @0x004258f0` is NOT used by any of these 5 screens.

LABEL NOTE: button labels are `SNK_*ButTxt` C++ symbol exrefs resolved at runtime from LANGUAGE.DLL (English text not in the exe). Port hardcodes English literals. Where the literal differs from the symbol stem it is flagged.

---

### Screen 20 @ 0x0040dfc0 — CarSelectionScreenStateMachine  [interactive: Y]
Port handler: td5_frontend.c:8500 (Screen_CarSelection, registered td5_frontend.c:111 case TD5_SCREEN_CAR_SELECTION)
DISPATCH MODEL: index-switch @0x0040e6xx (case 7) — buttons created in state 4 @0x0040e0c8; interaction reads g_frontendButtonIndex 0..5 + g_postRaceRacerCardNavDirection. Working car index lives in the SHARED scratch global g_quickRaceSelectedTrackId (NOT the track), committed to g_selectedCarIndex/g_player1Selected* only at state 0x18/0x1a.

BUTTONS (creation order, state 4 @0x0040e0c8):
| # | label (SNK / English literal) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_CarButTxt / "Car" | N/A index | ◄► cycles working car index (g_quickRaceSelectedTrackId += dir); Masters(gt5) skips taken slots; wrap per game-type (gt2 0..0xf, gt8 0x21..0x24, else 0..maxUnlocked); →state 10 reload preview | always | Implemented | td5_frontend.c:8670 (case 0) | Faithful: roster bounds + Masters skip + wrap replicated (:8552-8585, :8670-8688) |
| 1 | SNK_PaintButTxt / "Paint" | N/A index | ◄► cycles g_carSelectPaintSchemeTransient 0..3; INERT when car is cop (0x1c..0x24); →state 10 | always | Implemented | td5_frontend.c:8690 (case 1) | Faithful (cop-car gate :8695) |
| 2 | SNK_ConfigButTxt / **port label "Stats"** | N/A index | OPENS spec/config sub-screen: anim→state 0xf (config headers + per-car config.nfo values from g_localization*Scratch tables); ◄► here also re-enters 0xf | always | Partial | td5_frontend.c:8704 (case 2)→state 15 | Label literal "Stats" ≠ symbol stem "Config" (cosmetic). Functional sub-screen state 15 loads spec fields (:8806); the SNK_Info_Values sub-panel (orig state 0x11) is a NO-OP stub (:8814 "Language.dll unavailable") |
| 3 | SNK_AutoButTxt"Automatic" / SNK_ManualButTxt"Manual" (preview/greyed when gt7 TimeTrials) | N/A index | TOGGLES g_carSelectManualTransmissionToggle ^=1, RebuildFrontendButtonSurface(3) relabels | created as PreviewButton (greyed) when gt==7 | Implemented | td5_frontend.c:8634 create, :8709 (case 3) toggle | Faithful: gt7 forces Manual + greyed (:8634 ternary, :8710 gate) |
| 4 | SNK_OkButTxt / "OK" | N/A index | ACCEPT: lock-check (cop/cheat/gt8/gt5 bypass) → g_returnToScreenIndex=-1, state 0x14 slide-out; Masters marks slot taken=2; locked plays sfx(10) | always | Implemented | td5_frontend.c:8719 (case 4) | Faithful incl. lock enforcement (:8724) + Masters taken-flag (:8732) |
| 5 | SNK_BackButTxt / "Back" | N/A index | BACK: g_returnToScreenIndex=6, state 0x14 | created ONLY when g_postRaceRestartSelectedRace==0 && g_mainMenuFlowPhase==0 (else esc=btn4, no Back) | Implemented | td5_frontend.c:8637 create, :8740 (case 5) | Port gates create on `!s_network_active` (:8637); orig gates on restart/flow phase — DIVERGENT gate condition (see GAPS). Back routes RACE_TYPE_MENU not screen 6 directly |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Car preview render panel (CarPic_%d.tga, 408×280) | per-car image, slide-out/in on cycle (states 10/11/0xf via DrawCarSelectionPreviewOverlay 0x40ddc0) | g_carSelectionFrameAccumulator, car=g_quickRaceSelectedTrackId, paint transient | Implemented | td5_frontend.c:8772 (state 11 slide) / frontend_load_car_preview_surface | Faithful slide timing (13f out / 25f in) |
| CarSelBar1/CarSelCurve/CarSelTopBar overlays | static sidebar chrome, slide-in state 2 | s_carsel_*_surface | Implemented | td5_frontend.c:8520-8522 | Faithful |
| Blue car-bg fill (orig FillPrimaryFrontendRect 0x5c) | solid blue behind preview | — | Implemented (arch-div impl) | td5_frontend.c:8530-8548 (1×1 BGRA 0xFF00005C tex) | Renderer clears per frame; orig fills primary directly. Equivalent |
| Car NAME text strip ("%s %s") | centered car name, redrawn on cycle | local_80 from car name table | Implemented | frontend_render path | Faithful |
| Beast/Beauty/Locked tag (gt2 / locked) | "Beauty" if car<8 else "Beast" (Era); "Locked" overlay | SNK_Beast/Beauty/LockedTxt | [UNCERTAIN] | not located in case 7/15 port | Era Beast/Beauty tag + Locked watermark not clearly reproduced in port draw |
| Spec/Config stat sheet (state 0xf) | SNK_Config_Hdrs[] column + per-car config.nfo value columns | g_localizationCupTitleScratch / TrackNameScratch / OptionLabelScratch | Implemented | td5_frontend.c:8806 (state 15) frontend_load_car_spec_fields | Faithful enough; header/value layout from config.nfo |
| Info_Values sub-panel (state 0x11) | 10 LANGUAGE.DLL strings, centered | SNK_Info_Values | Missing (stub) | td5_frontend.c:8814 (case 17 no-op) | LANGUAGE.DLL strings unavailable → panel empty |

PARITY VERDICT: Faithful — full 26-state FSM, button order/gating, selector cycling, lock enforcement, Masters/2P/drag/cup exit routing all ported.
GAPS (actionable):
- Tab-3 button label literal is "Stats" in port but underlying orig symbol stem is "Config" (`SNK_ConfigButTxt`) — verify intended UI text vs LANGUAGE.DLL English.
- Back-button CREATE gate differs: orig = `restart==0 && flowPhase==0`; port = `!s_network_active` (td5_frontend.c:8637). Could create/omit Back in the wrong contexts (post-race restart / main-menu-flow).
- State 0x11 Info_Values sub-panel is a no-op stub (LANGUAGE.DLL) — empty info page.
- Era Beast/Beauty + Locked overlay tags ([UNCERTAIN]) not clearly drawn.
Confidence: [CONFIRMED @ 0x0040dfc0 main + 0x0040ddc0 sub] / [UNCERTAIN: Beast/Beauty/Locked tag port draw path; exact Back-gate behavioral impact]

---

### Screen 21 @ 0x00427630 — TrackSelectionScreenStateMachine  [interactive: Y]
Port handler: td5_frontend.c:9019 (Screen_TrackSelection, case TD5_SCREEN_TRACK_SELECTION)
DISPATCH MODEL: index-switch @0x004279xx (case 4) — buttons created state 0; reads g_frontendButtonIndex 0..3 + g_postRaceRacerCardNavDirection. Selected track = g_selectedScheduleIndex; direction = _g_selectedTrackDirection.

BUTTONS (creation order, state 0):
| # | label (SNK / English) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_TrackButTxt / "Track" | N/A index | ◄► cycles g_selectedScheduleIndex ±dir; skips locked NPC groups (g_npcRacerGroupTable &3) for gt>7; wrap [0..maxTrack]/(2P -1..); →state 5 reload preview | always | Implemented | td5_frontend.c:9083 (case 4 sel==0) | Faithful: cycle + skip-absent-level + 2P random(-1). Lock-group skip approximated via level-exists check (:9093) |
| 1 | SNK_ForwardsButTxt / "Forwards"↔"Backwards" | N/A index | TOGGLES _g_selectedTrackDirection ^=1, RebuildFrontendButtonSurface(1); ONLY if track has reverse flag (gNpcRacerCheatFlags[idx]!=0) | INERT/sprite-hidden (x=-0xe0) when track lacks reverse | Implemented | td5_frontend.c:9119 (case 4 sel==1) + :8997 visibility helper | Faithful: port hides+disables on forward-only tracks via td5_asset_track_has_reverse (:9002); orig gates on per-track reverse byte |
| 2 | SNK_OkButTxt / "OK" | N/A index | LAUNCH: lock-check (locked→sfx10); else g_returnToScreenIndex=~LOCALIZATION (=-1 launch), state 6 slide-out | esc-button when flow context==2 (QuickRace) | Implemented | td5_frontend.c:9128 (case 4 idx==2) | Faithful incl. lock check (:9130) + g_td5.reverse_direction commit (:9136) |
| 3 | SNK_BackButTxt / "Back" | N/A index | BACK: g_returnToScreenIndex=CAR_SELECTION, state 6 | created ONLY when g_mainMenuButtonHint != 2 (QuickRace context omits Back, esc=btn2) | Implemented | td5_frontend.c:9044 create gate (s_flow_context!=2), :9142 (idx==3) | Faithful gate (:9044) |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Track thumbnail (Tracks/%s.tga, 152×224) | per-track preview image, slide on cycle | DAT_004a2c94 | Implemented | td5_frontend.c:9053/9153 frontend_load_selected_track_preview | Faithful |
| Track name plate (296×64) | track name text (parsed up to ',' from SNK_TrackNames) | g_lobbyErrorDialogSurface | Implemented | frontend_get_track_name | Faithful |
| Track description box (224×120, src_y=0x40) | second region of dialog surface | g_lobbyErrorDialogSurface | Implemented | render path | Faithful |
| "Locked" overlay (locked track) | drawn into 0x98-wide region when track locked & no cheat | SNK_LockedTxt | [UNCERTAIN] | lock enforced on OK (:9130) but overlay-text draw not confirmed | Lock TEXT overlay on preview may be missing (lock still enforced functionally) |

PARITY VERDICT: Faithful — 9-state FSM, 4 buttons w/ correct order+gating, track cycler, reverse-toggle hide-on-forward-only, OK/Back routing all ported.
GAPS (actionable):
- "Locked" text overlay on the track preview ([UNCERTAIN]) may not draw (functional lock OK).
- Orig skips locked NPC race-groups via g_npcRacerGroupTable bit test (gt>7 cup modes); port substitutes a level-zip-exists check — close but not identical for cup track schedules.
Confidence: [CONFIRMED @ 0x00427630] / [UNCERTAIN: Locked-text overlay draw; NPC-group-skip vs level-exists equivalence]

---

### Screen 22 @ 0x00417d50 — ScreenExtrasGallery  [interactive: Y (any-key/ESC advances/quits)]
Port handler: td5_frontend.c:9226 (Screen_ExtrasGallery, case TD5_SCREEN_EXTRAS_GALLERY)
DISPATCH MODEL: callback/edge-poll @0x00418062 (case 7) — NO buttons. Exit on (g_frontendInputEdgeBits & 0x40000) || g_frontendMouseEdgeBits || credit-page-counter DAT_00496354==0xb → DXWin::CleanUpAndPostQuit() (quits the GAME). Scroll driven by g_frontendAnimFrameCounter 0..0x27f wrapping a 0x280-tall two-half credits surface.

BUTTONS: NONE.

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Continuous credits SCROLL (320×320 viewport @ x=0xcc y=0x60) | vertically scrolling text + mugshot rows, wrapping two 0x140 halves of g_secondaryWorkSurface; advances g_extrasGalleryPage per row, mugshot every 7th | g_frontendAnimFrameCounter, g_extrasGalleryPage, g_extrasGalleryAssetHandle | Wrong (structural) | td5_frontend.c:9251 (case 2) | Port shows ONE full-screen still image at a time, auto-advancing every 4000ms (:9256) — NOT the orig's continuous vertical credit scroll w/ interleaved text rows + 7-mugshot blocks. [UNCERTAIN duration: port comment flags 4000ms as guessed] |
| 27 mugshot TGAs (Bob..Daz) + 5 Legals TGAs | mugshot row blit (color-keyed 0x11) interleaved with credit text rows; loaded ALL at init | _DAT_004962e0.._DAT_00496348 | Partial | td5_frontend.c:9217 s_gallery_names[27] | Port loads on-demand one at a time (VRAM save) + drops the 5 Legals as separate scroll frames into the same 27-list; credit TEXT rows (SNK_CreditsText) entirely absent |
| Credit text rows (SNK_CreditsText[page*0x18]) | per-line localized credit text, '#'=mugshot marker, '*'=page-end | SNK_CreditsText | Missing | — | LANGUAGE.DLL credit strings not drawn; port is image-only |
| ESC / any-key / page==0xb → quit game | exit handler | g_frontendInputEdgeBits 0x40000 | Partial | td5_frontend.c:9265 frontend_post_quit | Port quits after last IMAGE (index>=27) or via global ESC handler; orig quits on page-counter 0xb OR any input edge |

PARITY VERDICT: Divergent — port reimplements the credits roll as a paged still-image slideshow; the original's continuous scrolling text+mugshot credit roll is not reproduced.
GAPS (actionable):
- No scrolling credit TEXT (SNK_CreditsText) — port shows only mugshot/legal images.
- Auto-advance interval (4000ms) is a guess (port comment [UNCERTAIN]); orig uses frame-counter scroll, not a fixed per-image timer.
- Image ordering differs (port linear list vs orig text-driven interleave w/ '#' mugshot markers).
Confidence: [CONFIRMED @ 0x00417d50] / [UNCERTAIN: orig per-image dwell time in real units; whether credit-text omission is intentional ARCH-DIV]

---

### Screen 23 @ 0x00413580 — ScreenPostRaceHighScoreTable  [interactive: Y]
Port handler: td5_frontend.c:9290 (Screen_PostRaceHighScore, case TD5_SCREEN_HIGH_SCORE)
DISPATCH MODEL: index-switch @0x004138xx (case 6) — 2 buttons; reads g_frontendButtonIndex (0=nav-bar, 1=OK) + g_postRaceRacerCardNavDirection. Browsed category = g_postRaceRacerCardIndex.

BUTTONS (creation order, state 0):
| # | label (SNK / English) | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | NULL label (520×32) | N/A index | NAV BAR: ◄► cycles g_postRaceRacerCardIndex ±dir across score categories (wrap, cheat-gated unlock range: normal [0..musicTrackIdx)/[0x13..0x19], cheat [0..0x19]); RebuildButtonSurface(0) + DrawPostRaceHighScoreEntry(idx) | always; idx-0 dispatch only when nav != 0 | Implemented | td5_frontend.c:9299 create (NULL), :9327 (case 6 delta) | Port wrap is simple [0..0x19] (:9334); orig has cheat-gated dual-range wrap (locked-cup skip). DIVERGENT wrap range — port lets you browse locked categories |
| 1 | SNK_OkButTxt / "OK" | N/A index | EXIT: g_returnToScreenIndex=MAIN_MENU, blit secondary→primary, state 7 slide-out | always | Implemented | td5_frontend.c:9337 (idx==1) | Faithful; esc=btn1 |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| High-score table panel (520×144) | per-category name/time rows, filled by DrawPostRaceHighScoreEntry @0x413010 | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c render + score-entry draw | Faithful (DrawPostRaceHighScoreEntry not deep-decompiled; row layout assumed faithful) |
| Header label (title surface) | "High Scores" screen title, slide-in | g_currentScreenIndex (CreateMenuStringLabelSurface(7)) | Implemented | td5_frontend.c:737 (FE_TITLE_PAGE_BASE+6) | Faithful |
| State-7 exit flush | writes config (high-score table) when return==-1 | — | Implemented | td5_frontend.c:9356 td5_save_write_config | Faithful (orig InitializeFrontendDisplayModeState → WritePackedConfigTd5) |

PARITY VERDICT: Faithful — 9-state FSM, 2 buttons (NULL nav bar + OK), category browse, config flush on exit.
GAPS (actionable):
- Category-browse wrap range differs: port = plain [0..0x19] (td5_frontend.c:9334); orig = cheat-gated dual range that skips locked cups. Port exposes locked categories in the browser.
Confidence: [CONFIRMED @ 0x00413580; DrawPostRaceHighScoreEntry 0x413010 not deep-decompiled (one-level budget)]

---

### Screen 24 @ 0x00422480 — RunRaceResultsScreen  [interactive: Y]
Port handler: td5_frontend.c:9375 (Screen_RaceResults, case TD5_SCREEN_RACE_RESULTS)
DISPATCH MODEL: index-switch — TWO distinct button sets in two FSM phases. Phase A (results table, states 4-0xb): 2 buttons (NULL panel + OK) + ◄► slot browse. Phase B (post-race menu, state 0xd build → 0xf dispatch): 5 context-dependent buttons dispatched via g_postRaceMenuButtonChoice = g_frontendButtonIndex in state 0x10. Browsed racer = g_postRaceRacerCardIndex.

BUTTONS — Phase A (state 0 @0x004227xx):
| # | label / English | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | NULL (520×32) | N/A index | ◄► browse racer slots 0-5 (skip state==3 disabled; drag gt7 mask &1): right→state 7, left→state 9 slide-out, cycle mid-slide, re-fill via DrawRaceDataSummaryPanel | always | Implemented | td5_frontend.c:9519 (case 6 arrow) | Faithful incl. slot-skip + drag mask (:9567) |
| 1 | SNK_OkButTxt / "OK" | N/A index | CONFIRM (idx 0..1) → state 0x0b exit slide-out | always; esc=btn0 | Implemented | td5_frontend.c:9545 (idx<2) | Faithful |

BUTTONS — Phase B post-race menu (state 0xd @0x004231d6, 5 buttons; labels differ cup vs single):
| # | label single (gt0/7/9) / label cup (gt1-6) | userdata | action (state 0x10 dispatch on choice) | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 0 | SNK_RaceAgain"Race Again" / SNK_NextCupRace"Next Cup Race" | N/A index | choice0: restore snapshot, cup→race_within_series++, InitializeFrontendDisplayModeState (re-race) | NextCupRace is PreviewButton(greyed) when ConfigureGameTypeFlags()==0 (cup complete) | Implemented | td5_frontend.c:9714 label, :9763 dispatch | Faithful |
| 1 | SNK_ViewReplay / "View Replay" | N/A index | choice1: g_inputPlaybackActive=1, replay mode, re-init race | PreviewButton(greyed) when g_replayFileAvailable==0 | Implemented | td5_frontend.c:9721 create, :9777 dispatch | Faithful (sets replay+playback+game replay flag :9791-9793) |
| 2 | SNK_ViewRaceData / "View Race Data" | N/A index | choice2: SetFrontendScreen(0x18=self) — re-enter screen 24 from state 0 | PreviewButton(greyed) when no race data (slot 0x1f2 size_rate==2 \|\| pool empty) | Implemented | td5_frontend.c:9722 create, :9797 dispatch self-jump | Faithful self-jump |
| 3 | SNK_SelectNewCar"Select New Car" / SNK_SaveRaceStatus"Save Race Status" | N/A index | choice3: cup→state 0x11 WriteCupData; single→CAR_SELECTION | SaveRaceStatus PreviewButton(greyed) when cup complete path | Implemented | td5_frontend.c:9715 label, :9822 dispatch | Faithful |
| 4 | SNK_Quit"Quit" / SNK_OkButTxt"OK"(cup-complete) | N/A index | choice4: gt<1→AwardCupUnlocks+NAME_ENTRY(0x19); cup mid→MAIN_MENU; cup-done won(pos==0)→CUP_WON(0x1b); failed→CUP_FAILED(0x1a) | label swaps Quit→OK when cup complete (next_valid==0) | Implemented | td5_frontend.c:9718 label, :9830 dispatch | Faithful 4-branch Quit (:9855-9866) |

Phase B-aux: state 0x11 save dialog = 2 buttons (msg SNK_BlockSavedOK/SNK_FailedToSave + OK) td5_frontend.c:9887. State 0x13 OK exits to menu.

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Results data panel (408×392) | per-racer position/time/speed rows filled by DrawRaceDataSummaryPanel @0x421e90; slides L/R on browse | g_lobbyErrorDialogSurface, g_postRaceRacerCardIndex | Implemented | td5_frontend.c:4770 frontend_render_race_results_overlay | Faithful slide formulas (states 7/8/9/10 confirmed in port :9554-9642); panel rendered fresh per-frame (arch-div: not a tracked surface) |
| Per-game-type result label ladder (case 0 loops) | DrawFrontendLocalizedStringToSurface y-loops differ by gt (0/7/8/9 + cup 1-6) | gt-dependent | Implemented | td5_frontend.c:4848 label ladder | Faithful per-gt ladder (CONFIRMED comment :4848) |
| Header/title (CreateMenuStringLabelSurface(0xe)) | "Results" title slide | g_currentScreenIndex | Implemented | td5_frontend.c:738 / title gating :5779 | Faithful (P1 title-strip gating to states 3..0xb :5779) |
| Cup-fail early-route (state 0) | gt4 + flag → CUP_FAILED before table | g_raceParticlePoolBase[0x20c] flag | Implemented | td5_frontend.c:9398 | Faithful early-route |

PARITY VERDICT: Faithful — full 22-state FSM (0x00-0x15), dual button phases, racer-slot browser w/ skip+drag-mask, context menu w/ greyed PreviewButtons, 4-branch Quit, cup-save dialog, replay/view-data self-jump, snapshot save/restore all ported.
GAPS (actionable):
- DrawRaceDataSummaryPanel 0x421e90 + DrawPostRaceHighScoreEntry internal row layouts not deep-decompiled (one-level budget) — assumed faithful, not byte-verified.
- 2P-mode result backup/restore block (orig g_postRaceCarSelectionBackup, big struct copy) is large; port has s_snap_* — coverage looks present but the gt7 secondary-field copy (orig case 0xf gt7 branch) is dense and not line-verified.
Confidence: [CONFIRMED @ 0x00422480 main] / [UNCERTAIN: summary-panel internal layout; 2P/gt7 backup-struct field-by-field parity]

---

## Cross-screen summary
- Interactive: 20 Y, 21 Y, 22 Y(no buttons, edge-exit), 23 Y, 24 Y.
- Dispatch model: ALL index-switch (g_frontendButtonIndex by creation order) + ◄► via g_postRaceRacerCardNavDirection — NO userdata, NO callback ptr. Port mirrors exactly (frontend_create_button has no ud; s_button_index/frontend_option_delta switch).
- Worst parity: Screen 22 (Divergent — paged still-image slideshow vs continuous credit scroll, no SNK_CreditsText).
- Best parity: Screens 23, 24, 20, 21 (Faithful FSMs).
# Frontend BEHAVIOR Harvest — Slice 25 (g_frontendScreenFnTable indices 25–29)

Source: deterministic decompilation of `TD5_d3d.exe` via Ghidra (read_only, session reuse of an
already-open TD5_d3d.exe pool program). Companion to the POSITION harvest in `part_25.md`; this file
records button ACTIONS, dispatch model, non-button element behavior, and port parity.

Conventions / shared facts (all CONFIRMED at the cited addresses):
- `CreateFrontendDisplayModeButton @ 0x00425de0 (label, x, y, w, h, userdata)` — every call in these 5
  screens passes `userdata = 0`. There is NO per-button userdata dispatch anywhere in slice 25. The
  original selects the active button by INDEX (`g_frontendButtonIndex`) and signals a press via the
  global `g_frontendButtonPressedFlag` (single bool, set by the shared input/cursor layer, NOT a
  per-button callback pointer). So the dispatch model for the whole slice is **index/press-flag**, not
  userdata and not callback-ptr. The port mirrors this: `frontend_create_button(label,x,y,w,h)` (NO
  userdata param, td5_frontend.c:1315) + per-screen `switch(s_inner_state)` reading `s_input_ready`
  (td5_frontend.c:180, the port's `g_frontendButtonPressedFlag` analog).
- SNK_* labels are C++ mangled symbols (e.g. `?SNK_OkButTxt@@3PADA` @ 0x00460b96) → runtime pointers
  into LANGUAGE.DLL, not literals in the exe. English values are taken from the project-confirmed
  LANGUAGE.DLL SNK_ tables (see MEMORY.md keyboard-rebind note for the SNK→English provenance).
- `g_frontendButtonPressedFlag` press → on dialogs (26/27/29) navigates to `g_returnToScreenIndex`
  (26/27) or hard-coded MAIN_MENU (29). NameEntry(25) advances its own inner FSM.

g_frontendScreenFnTable @ 0x004655C4 indices 25–29:
25 → 0x00413bc0 ScreenPostRaceNameEntry
26 → 0x004237f0 ScreenCupFailedDialog
27 → 0x00423a80 ScreenCupWonDialog
28 → 0x00415370 ScreenStartupInit
29 → 0x0041d630 ScreenSessionLockedDialog

---

### Screen 25 @ 0x00413bc0 — ScreenPostRaceNameEntry  [interactive: Y]
Port handler: td5_frontend.c:9929 (case TD5_SCREEN_NAME_ENTRY, Screen_PostRaceNameEntry)
DISPATCH MODEL: index/press-flag @0x00413bc0 — 13-case inner FSM (g_frontendInnerState 0–0xC). Input
is a FREE-FORM TEXT WIDGET (NOT an alphabet grid), then a one-button OK confirm on the score table.

INPUT MODEL (the load-bearing behavioral finding): the name is captured by
`RenderFrontendCreateSessionNameInput @ 0x0041a530` (called every frame in case 2). It is a DirectInput
ANSI string capture: `DXInput::SetAnsi(1)` + `DXInput::GetString(&g_postRaceNameEditTargetPtr)` writing
directly into `g_postRacePlayerNameEntryBuffer` (max len 0x10/16, set at case 0 via
`_g_postRaceNameEditMaxLength=0x10`, `g_postRaceNameEditTargetPtr=&g_postRacePlayerNameEntryBuffer`).
There is NO on-screen letter grid and NO per-letter selection/confirm. A blinking caret is drawn by
filling a 2×0x10 green (0xff00) rect at the measured text-width x, gated on
`(g_frontendAnimFrameCounter & 0x20)==0` and `g_postRaceNameEditState!=2`. Confirm = the input layer
sets `g_postRaceNameEditState==2` (Enter), which case 2 detects → advances to slide-out (case 3) →
insert (case 4). If the entered buffer is empty (length wraps to -2 in the do/while strlen at case 2),
the code copies `g_localComputerName` into the name buffer as the default.

FSM flow: case0 qualification check → if not qualifying, jumps straight to case4 (silent insert,
score=0 path) → main menu; if qualifying, case1 slide-in → case2 type name → case3 slide-out → case4
insert into per-schedule high-score table (find pos, shift, write name/score/car/avg/top) → case5/6
build score-table surface via DrawPostRaceHighScoreEntry(g_selectedScheduleIndex) → case7 table
slide-in → case8/9/10 settle + wait OK → case0xB rebuild → case0xC slide-out → MAIN_MENU.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_EnterPlayerNameButTxt ("ENTER PLAYER NAME") | 0 | NOT a press target — it is the baked title strip of the name-input surface (CreateFrontendDisplayModeButton(-0x1c0,0,0x1c0,0x40); g_postRaceNameButtonSurfacePtr). Confirm comes from text-widget Enter, not a click. | case 0, only if score!=0 & not 2P & qualifies | Partial | td5_frontend.c:2499-2506 (live "ENTER PLAYER NAME" title) + 2470 frontend_render_text_input | Port draws title as live text above a live input box (not a baked button surface); functionally equivalent, cosmetically different. |
| 2 | SNK_OkButTxt ("OK") | 0 | Confirm score-table display → begin slide-out (case0xC) → MAIN_MENU. Selected by index (g_frontendButtonIndex=1); press via g_frontendButtonPressedFlag at case 10. | case 4 onward (score-table phase) | Implemented | td5_frontend.c:10162 (case 10 waits s_input_ready) | Port has no explicit "OK" button object for this phase — it advances on s_input_ready (any confirm). Faithful in effect. |
| – | (NULL backing strip) -0x208×0x20 | 0 | Pure visual backing rect behind OK (RebuildFrontendButtonSurface). Not interactive. | case 4 | Missing (cosmetic) | — | Port omits the 520×32 backing strip behind OK; minor visual. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Name text field + caret | DirectInput ANSI capture into 16-byte buffer; blinking caret | g_postRacePlayerNameEntryBuffer / g_postRaceNameEditState | Partial | td5_frontend.c:2410 frontend_handle_text_input_key (GetAsyncKeyState, not DXInput) | Port re-implements with Win32 GetAsyncKeyState + MapVirtualKey instead of DXInput::GetString; default-on-empty uses "PLAYER" (10022) not g_localComputerName (orig). |
| High-score table surface | 520×144 black surface, 5 rows + headers (NAME/BEST-TIME/LAP/PTS/CAR/AVG SPD/TOP SPD) drawn by DrawPostRaceHighScoreEntry @0x413010; highlighted row = g_postRaceQualifyingScore (the just-inserted index) using bold font g_smallTextbSurface | g_lobbyErrorDialogSurface / g_postRaceQualifyingScore | Partial | td5_frontend.c:4649 frontend_render_high_score_overlay (dispatched at 5563) | Port renders live and faithfully columns; BUT highlight is hard-coded row 0 (4716 `i==0`) instead of orig's inserted-position row (g_postRaceQualifyingScore). WRONG if score lands at rank 2–5. |
| Slide sprites (slot 0/1) | header/decoration sprites animated via MoveFrontendSpriteRect | sprite slots | Missing (cosmetic) | — | Port has no equivalent sprite-slide decoration; slide is abstracted to s_anim_tick. |
| Header label surface | g_currentScreenIndex screen-title strip (CreateMenuStringLabelSurface(7)) | g_currentScreenIndex | Partial | td5_frontend.c title-page path (730+) | Title page index handled but header-strip blit is abstracted. |

PARITY VERDICT: Partial — FSM, qualification math, table insert/shift, and confirm flow are faithfully
ported (cases 0/4 heavily CONFIRMED-annotated); divergences are (a) text widget re-implemented via
Win32 not DXInput, (b) empty-name default "PLAYER" vs orig g_localComputerName, (c) high-score highlight
row hard-coded to 0 instead of the inserted rank.
GAPS (actionable):
- Highlight WRONG: td5_frontend.c:4716 uses `i==0`; should highlight `s_score_insert_pos` row (orig
  g_postRaceQualifyingScore) so a rank-3 finish bolds row 3, not row 1.
- Empty-name default: orig copies g_localComputerName (machine name) when the field is left blank;
  port uses literal "PLAYER" (td5_frontend.c:10022). Cosmetic divergence.
- Input backend: orig DXInput::GetString ANSI; port GetAsyncKeyState. Behaviorally close; no IME / no
  non-A–Z0–9/space chars in port (td5_frontend.c:2434 loops VK 0x30–0x5A only).
Confidence: [CONFIRMED @ 0x00413bc0, sub 0x00413010, input widget 0x0041a530]. [UNCERTAIN: exact
LANGUAGE.DLL English for SNK_EnterPlayerNameButTxt — assumed "ENTER PLAYER NAME".]

---

### Screen 26 @ 0x004237f0 — ScreenCupFailedDialog  [interactive: Y]
Port handler: td5_frontend.c:10196 (case TD5_SCREEN_CUP_FAILED, Screen_CupFailed)
DISPATCH MODEL: index/press-flag @0x004237f0 — 6-case FSM; ONE button (OK). Dialog visible states 4–5.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: if g_frontendButtonPressedFlag → release dialog surface + buttons → SetFrontendScreen(g_returnToScreenIndex). g_frontendEscKeyButtonIndex=0 (Esc also confirms). | dialog shown only if 0<gameType<7; else case0 → MAIN_MENU immediately | Implemented | td5_frontend.c:10218 (create) + 10236 (case 5 → MAIN_MENU on s_input_ready) | Port navigates to hard MAIN_MENU; orig goes to g_returnToScreenIndex. For this dialog return screen is initialized to MAIN_MENU's value at entry (frontend_init_return_screen), so equivalent in practice. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Dialog surface 408×112 | black-fill surface, 4 centered text lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5339 frontend_render_cup_failed_overlay | Live-drawn quad+text vs baked surface; faithful position (cx-0xa8, cy-0x8f). |
| "SORRY" (y0) / "YOU FAILED" (y0x1c) / "TO WIN" (y0x38) | SNK_SorryTxt / SNK_YouFailedTxt / SNK_ToWinTxt | — | Implemented | td5_frontend.c:5359-5363 | Faithful y offsets 0/28/56. |
| cup-type name (y0x54) | *(SNK_RaceTypeText + gameType*4) → 1=CHAMPIONSHIP..6=ULTIMATE CUP | g_frontendSelectedGameType | Implemented | td5_frontend.c:5365-5368, k_cup_type_names[] | Faithful; cup names CONFIRMED from Language.dll. |
| slide sprite slot 0 | decoration slide-in | sprite slot 0 | Missing (cosmetic) | — | Port abstracts to s_anim_tick; no sprite. |

PARITY VERDICT: Faithful — single-OK dialog, gating (cup types 1–6 only), text lines, and confirm→exit
all match. Only cosmetic sprite-slide and baked-vs-live surface differ.
GAPS (actionable):
- Return target: port hard-codes MAIN_MENU (10239) vs orig g_returnToScreenIndex; harmless because
  this screen's return is always MAIN_MENU, but a literal-faithful port would use g_returnToScreenIndex.
Confidence: [CONFIRMED @ 0x004237f0]. [UNCERTAIN: none material.]

---

### Screen 27 @ 0x00423a80 — ScreenCupWonDialog  [interactive: Y]
Port handler: td5_frontend.c:10251 (case TD5_SCREEN_CUP_WON, Screen_CupWon)
DISPATCH MODEL: index/press-flag @0x00423a80 — 6-case FSM; ONE button (OK). Dialog 408×196.

SIDE EFFECTS at case 0 (CONFIRMED): `_unlink(g_cupDataTd5Filename)` deletes CupData.td5 (cup
progress wiped on win). Then loads MainMenu bg, allocs 0x198×0xc4 surface, draws text.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: g_frontendButtonPressedFlag → release surface/buttons → SetFrontendScreen(g_returnToScreenIndex). Esc also confirms (g_frontendEscKeyButtonIndex=0). | dialog only if 0<gameType<7 | Implemented | td5_frontend.c:10313 (create) + 10330 (case 5 → MAIN_MENU on s_input_ready) | Same MAIN_MENU-vs-g_returnToScreenIndex note as 26. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Dialog surface 408×196 | black-fill, taller than 26/29 for extra lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5394 frontend_render_cup_won_overlay | Faithful 0xc4 height. |
| "CONGRATULATIONS" (y0) / "YOU HAVE WON" (y0x38) | SNK_CongratsTxt / SNK_YouHaveWonTxt | — | Implemented | td5_frontend.c:5412-5414 | Faithful y 0/56. |
| cup-type name (y0x54) | *(SNK_RaceTypeText + gameType*4) | g_frontendSelectedGameType | Implemented | td5_frontend.c:5416-5419 | Faithful. |
| car-unlock line (y0x8c=140) | sprintf "%d %s" (count + SNK noun); gated on DAT_00494bb0 (car count) != 0 | DAT_00494bb0 | Partial | td5_frontend.c:5421-5424 ("%d NEW CARS UNLOCKED", s_cup_won_car_count) | Gating + count CONFIRMED-correct (disasm 0x423bb1 TEST/JZ on [0x494bb0]); only the SNK noun wording is UNCERTAIN. |
| track-unlock line (y0xa8=168) | sprintf "%d %s"; gated on DAT_00494bb4 (track count) != 0 | DAT_00494bb4 | Partial | td5_frontend.c:5426-5429 ("%d NEW TRACKS UNLOCKED", s_cup_won_track_count) | Same: count/gate correct, wording UNCERTAIN. |
| slide sprite slot 0 | decoration slide-in (settle y = ch+0x61 vs 26's ch+0x31) | sprite slot 0 | Missing (cosmetic) | — | No sprite in port. |

PARITY VERDICT: Faithful (functional) — FSM, CupData.td5 deletion (frontend_delete_cup_data
td5_frontend.c:10261), unlock-progression apply (10279), and the two count-gated unlock lines all
match the orig. The decompiler's `g_cupSchedule_currentCup/currentRound` variable names are MISNAMES;
disassembly proves the gates are on car/track counts ([0x494bb0]/[0x494bb4]) exactly as the port does.
GAPS (actionable):
- Unlock-line wording: orig `"%d %s"` with %s = SNK localized noun ([0x45d174]/[0x45d178]); port uses
  literal "NEW CARS UNLOCKED"/"NEW TRACKS UNLOCKED". Exact orig nouns UNCERTAIN (Language.dll not
  decompiled) — confirm against Language.dll for byte-exact wording.
- Return target MAIN_MENU vs g_returnToScreenIndex (same harmless note as 26).
Confidence: [CONFIRMED @ 0x00423a80; unlock gating disasm 0x423bb1/0x423c04]. [UNCERTAIN: SNK unlock-line
nouns at 0x0045d174 / 0x0045d178.]

---

### Screen 28 @ 0x00415370 — ScreenStartupInit  [interactive: N]
Port handler: td5_frontend.c:10343 (case TD5_SCREEN_STARTUP_INIT, Screen_StartupInit)
DISPATCH MODEL: index/press-flag @0x00415370 — 5-case bootstrap FSM. NOT interactive: the OK button it
creates is NEVER consumed (no g_frontendButtonPressedFlag check anywhere); case 4 self-advances by tick
count, releases the surface, and hands control to table[0] then re-seeds g_frontendInnerState with
timeGetTime(). It is a 4-tick blank-dialog transition placeholder, not a user prompt.

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 (port -100) | 0 | INERT — created at case0 but never read; auto-advance ignores it. | always | Implemented (inert, matches orig inertness) | td5_frontend.c:10349 frontend_create_button("OK",-100,...) | Port uses x=-100/w=100 vs orig x=-0x120(-288)/w=0x60(96). Cosmetically off, but button is invisible/inert so impact = none. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Blank dialog surface 408×112 | black-fill box, NO text drawn, shown 3 ticks | g_lobbyErrorDialogSurface | Partial | td5_frontend.c:10353-10355 (frontend_present_buffer, no quad) | Orig blits a visible black box for 3 frames; port presents buffer without drawing the box. Sub-frame flash; negligible. |
| Control hand-off | case4: g_currentScreenFnPtr = table[0]; g_frontendInnerState = timeGetTime() | g_currentScreenFnPtr | Wrong-target | td5_frontend.c:10360 → TD5_SCREEN_LOCALIZATION_INIT | Orig jumps to table[0] (g_frontendScreenFnTable base = index 0). Port redirects to TD5_SCREEN_LOCALIZATION_INIT instead. [UNCERTAIN whether table[0] == LocalizationInit; if index 0 is a different screen this is a divergence.] |

PARITY VERDICT: Partial — bootstrap timing/inert-OK behavior matches, but the case-4 hand-off target
differs (orig table[0] vs port TD5_SCREEN_LOCALIZATION_INIT) and the visible blank box is dropped.
GAPS (actionable):
- Verify table[0] identity: orig sets g_currentScreenFnPtr = g_frontendScreenFnTable (index 0). Confirm
  whether port's index 0 == TD5_SCREEN_LOCALIZATION_INIT; if not, td5_frontend.c:10360 is a wrong target.
- Orig seeds g_frontendInnerState = timeGetTime() after the hand-off (an RNG/timing seed reuse of the
  inner-state field); port does not replicate this seed write. Likely inert but a literal divergence.
Confidence: [CONFIRMED @ 0x00415370]. [UNCERTAIN: identity of g_frontendScreenFnTable[0] vs the port's
TD5_SCREEN_LOCALIZATION_INIT redirect.]

---

### Screen 29 @ 0x0041d630 — ScreenSessionLockedDialog  [interactive: Y]
Port handler: td5_frontend.c:10378 (case TD5_SCREEN_SESSION_LOCKED, Screen_SessionLocked)
DISPATCH MODEL: index/press-flag @0x0041d630 — 6-case FSM; ONE button (OK). Structurally = screen 26
minus one text line; ALWAYS returns to MAIN_MENU (hard-coded, not g_returnToScreenIndex).

BUTTONS (original order):
| # | label | userdata | action | gating | PORT status | port ref | gap |
|---|---|---|---|---|---|---|---|
| 1 | SNK_OkButTxt ("OK") @ x=-0x120 | 0 | case 5: g_frontendButtonPressedFlag → release surface/buttons → SetFrontendScreen(TD5_SCREEN_MAIN_MENU). Esc confirms (g_frontendEscKeyButtonIndex=0). | always (dialog) | Implemented | td5_frontend.c:10392 (create) + 10409 (case 5 → MAIN_MENU on s_input_ready) | Faithful — orig itself hard-codes MAIN_MENU here. |

NON-BUTTON ELEMENTS:
| element | role/behavior | global reflected | PORT status | port ref | gap |
|---|---|---|---|---|---|
| Cached full-canvas bg blit | case0: BlitFrontendCachedRect(0,0,W,H) | cached bg | Partial | td5_frontend.c:10383 frontend_load_tga | Port reloads MainMenu.tga instead of blitting cached rect; visual result equivalent. |
| Dialog surface 408×112 | black-fill, 2 text lines | g_lobbyErrorDialogSurface | Implemented | td5_frontend.c:5441 frontend_render_session_locked_overlay | Faithful pos/size. |
| "SORRY" (y0) / "SESSION LOCKED" (y0x38) | SNK_SorryTxt / SNK_SeshLockedTxt | — | Implemented | td5_frontend.c:5455-5457 | Faithful y 0/56. |
| slide sprite slot 0 | decoration slide-in | sprite slot 0 | Missing (cosmetic) | — | No sprite in port. |

PARITY VERDICT: Faithful — identical FSM to 26, two-line text, OK→MAIN_MENU. Note (ARCH-DIVERGENCE,
documented at td5_frontend.c:10370): in the port this screen is reachable only by manually setting the
kicked flag because no compatible DXPTYPE peer exists; the FSM itself is faithful.
GAPS (actionable):
- None functional. Cosmetic: cached-rect bg vs tga reload; missing decoration sprite.
Confidence: [CONFIRMED @ 0x0041d630]. [UNCERTAIN: none material; SNK_SeshLockedTxt English assumed
"SESSION LOCKED".]
