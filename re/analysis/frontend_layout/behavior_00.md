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
