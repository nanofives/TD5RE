# Frontend Assumption Register — TD5RE Port

Exhaustive register of every ASSUMPTION / unverified value in the port's frontend code, for
driving toward "0 assumptions, 100% faithful." Built 2026-06-01 (RESEARCH ONLY — no code edits).

Files audited (worktree `fix-1780170781-137846-13319`):
- `td5mod/src/td5re/td5_frontend.c` (11193 lines — 30-screen FSM + all overlays)
- `td5mod/src/td5re/td5_frontend_button_cache.c` (396 lines)
- `td5mod/src/td5re/td5_platform_win32.c` (frontend-relevant constants only)

Cross-referenced: `re/analysis/frontend_snk_strings.md` (Language.dll SNK_* dump — NOTE: that dump
TRUNCATED multi-line SNK entries at the first NUL, so SNK arrays may have more lines than shown —
flagged below), `re/analysis/frontend_screens/screens_*.md`, `re/analysis/frontend_fixlist/fix_*.md`,
`re/analysis/frontend_complete_spec.md`, `re/analysis/frontend_rendering_model.md`,
`re/analysis/frontend_screens_decompiled.c`, `re/analysis/frontend_diff/diff_*.md`.

---

## TOTALS

**By category:**
| Category | Count |
|---|---|
| STRING-LABEL | 31 (incl. ~80 button labels collapsed into 1 systemic entry) |
| POSITION | 9 |
| SIZE | 4 |
| COLOR | 6 |
| TIMING | 7 |
| INDEX-MAP | 3 |
| BEHAVIOR | 15 |
| CONTRADICTION | 2 |
| OTHER | 3 |
| **TOTAL discrete entries** | **80** |

**By resolvability:**
| Resolvability | Count | Notes |
|---|---|---|
| RE-RESOLVABLE | 49 | Pin via a named Ghidra fn / SNK_ symbol / TGA header |
| USER-DECISION | 12 | ARCH-DIV substitutions or subsystems not ported |
| ALREADY-CONFIRMED-OK | 19 | Has a valid [CONFIRMED @addr] basis; checked, not a real gap |

The single biggest item is **systemic STRING-LABEL divergence**: ~80 `frontend_create_button("...")`
calls use mixed-case English ("Race Menu", "Quick Race", "Change Car", ...) while every confirmed
SNK_* label is ALL-CAPS ("RACE MENU", "QUICK RACE", "CHANGE CAR"). This is RE-RESOLVABLE for all of
them via the SNK_*ButTxt table already dumped (see CONTRADICTIONS + per-screen rows).

---

## CONTRADICTIONS (highest priority — confirmed-wrong values)

### C1 — SNK_LangDLL font-mode byte: port says 0x31, all RE says 0x30
- **file:line** `td5_frontend.c:6150-6152` (Screen 0 LocalizationInit)
- **Port claim:** `SNK_LangDLL_exref[8]` is a language-selection byte: `0x31=English, 0x32=French,
  0x33=German, 0x34=Italian, 0x35=Spanish` (MOVs at 0x4269DF–0x426A07).
- **Conflict:** SIX independent RE artifacts say the byte gate is **`== 0x30`** for the
  default/English 24×24-font path:
  - `frontend_rendering_model.md:143` (DrawFrontendLocalizedStringPrimary: "only when ... `SNK_LangDLL_exref[8]==0x30`")
  - `frontend_screens_decompiled.c:1782` (`if (SNK_LangDLL_exref[8] == (code)0x30)`)
  - `frontend_rendering_core.c:54,117` (`!= 0x30` / `== 0x30`)
  - `frontend_diff/diff_00.md:145` ("switches to 24×24 when `SNK_LangDLL[8]==0x30`")
  - `frontend_screens/screens_00.md:205`, `frontend_complete_spec.md:247`
  - `frontend_flow_model.md:68` ("BodyText.tga if LANGUAGE.DLL byte[8]==0x30 else SmallText2.tga")
- **Note:** the `[8]==0x30` byte (a font-mode/locale gate read by the font path) and the per-car
  `config.eng` language-index byte the localization loop uses MAY be two different things — the port
  comment conflates them. Either way the inline values (0x31 English etc.) are unverified.
- **Resolvability:** RE-RESOLVABLE. Read `ScreenLocalizationInit @0x004269D0` (MOVs at
  0x4269DF–0x426A07) AND `DrawFrontendLocalizedStringPrimary @0x004242b0` to settle which byte/value
  selects English and whether it is 0x30 or 0x31. Read the actual `SNK_LangDLL` export bytes
  ("LANGDLL 0 : ENGLISH/US") in Language.dll @ RVA 0x6030 to confirm `[8]`.

### C2 — Music Test track→band LUT disagrees with the band-NAME table it indexes
- **file:line** `td5_frontend.c:185` `k_music_track_to_band[12] = {1,3,4,4,2,0,0,1,3,4,4,4}` vs
  `td5_frontend.c:159` `k_music_test_band[12]` (per-track band names).
- **Conflict:** The cover-LUT (line 185) is keyed to the cover load order
  `0=FearFactory,1=GravityKills,2=JunkieXL,3=KMFDM,4=PitchShifter` (comment @184). Applying it:
  track 0→band1=GravityKills (matches `k_music_test_band[0]="GRAVITY KILLS"` ✓), track 1→band3=KMFDM
  (matches `[1]="KMFDM"` ✓), track 2→band4=PitchShifter (matches `[2]="PITCHSHIFTER"` ✓), track 4→
  band2=JunkieXL (matches `[4]="JUNKIE XL"` ✓), track 5→band0=FearFactory (matches `[5]="FEAR FACTORY"`
  ✓). So the two tables are actually CONSISTENT for the played track. BUT note the two are derived from
  DIFFERENT confirmed sources (185 from LUT@0x465e4c; 159 from PTR_s_GRAVITY_KILLS@0x465e1c) and only
  ONE has been verified end-to-end. Treat as a **latent consistency assumption** to re-verify rather
  than a hard contradiction. Demoted; listed here for visibility.
- **Resolvability:** RE-RESOLVABLE. Dump LUT @0x465e4c and the band ptr table @0x465e1c together;
  confirm the 12→5 mapping matches the 12 band-name strings byte-for-byte.

---

## USER-DECISION QUEUE (questions to ask the user)

1. **Button label case/text source (SYSTEMIC).** Port hardcodes ~80 mixed-case English button labels
   ("Race Menu", "Change Car", "Forwards", "Continue Cup"...). Confirmed SNK_*ButTxt are ALL-CAPS
   ("RACE MENU", "CHANGE CAR", "FORWARDS", "CONTINUE CUP"). The original renders the SNK_ strings via
   the button bake. **Q: Replace every hardcoded label with the exact SNK_ string (ALL-CAPS, from
   Language.dll), and load them from Language.dll at runtime for locale fidelity — or keep
   English-only hardcoded ALL-CAPS strings as an accepted port simplification?** (RE-RESOLVABLE part:
   the exact strings; USER part: runtime-DLL vs hardcoded.)

2. **Language.dll runtime loading vs hardcoded English tables.** MANY label/value/header tables are
   hardcoded English copies of SNK_ strings (`k_cup_type_names`, `k_results_labels`, `k_race_desc`,
   `k_stat_layout_types`, `k_stat_engine_types`, on/off/difficulty/dynamics/split/sfx-mode arrays,
   race-type descriptions). **Q: Wire a real Language.dll SNK_ string reader (so non-English builds
   work + guarantees byte-exact English), or accept hardcoded English as permanent?**

3. **`k_race_desc` multi-line descriptions (Screen 6).** `td5_frontend.c:4255-4280` contains a
   12×13 table of race-type description lines that the author admits were RE-CONSTRUCTED after the
   SNK dump truncated them. **Q: These are guessed/reconstructed text — accept the risk, or
   re-extract the true multi-line `SNK_RaceTypeText` entries from Language.dll first?** (Strongly
   recommend RE re-extraction; flagged USER because content currently can't be trusted.)

4. **`k_js_value_labels` joystick value names (Screen 18).** `td5_frontend.c:469-479` placeholders
   "Axis+/Axis-/Btn1..Btn7". Original cycles values 2..10 via `SNK_ControlText[value*16]` — i.e. the
   SAME action strings as keyboard (ACCELERATE..REAR VIEW, NONE, ACCEL/BRAKE). **Q: The original does
   NOT show "Axis+/Btn1" — it shows action names. Replace placeholders with `SNK_ControlText` slots
   2..10 ("ACCELERATE","BRAKE",...,"ACCEL/BRAKE")?** (RE-RESOLVABLE; USER only to confirm the
   semantic that joystick binding cycles ACTIONS not axis/button labels.)

5. **MainMenu button-x Frida override (126 vs 110).** `td5_frontend.c:3919` overrides the auto-layout
   x (110, from FE_BTN_LEFT_OFFSET=0xD2) to 126 (0xC2) for MainMenu only, citing a Frida capture.
   Auto-layout for every OTHER screen still uses 110. **Q: Is 126 correct for MainMenu (and why does
   it differ from the 0xD2 immediate)? Should the 0xC2/0xD2 discrepancy be reconciled by reading the
   actual placement immediates in each screen fn?** (Partly RE-RESOLVABLE; the Frida-vs-static
   mismatch is a judgment call.)

6. **MainMenu slide endpoints from Frida (dx -482→126, +608px).** `td5_frontend.c:3856-3875` pins
   the slide trajectory to a Frida log rather than the screen-fn immediates. Other screens use ±640.
   **Q: Accept Frida-derived endpoints, or transcribe the real per-screen slide formulas
   (`base ± counter*step`, steps 0x10/0x18/0x20/0x30/0x38) from each screen fn?**

7. **ExtrasGallery 4000ms per-image dwell.** `td5_frontend.c:9533-9535` "[UNCERTAIN: original uses
   per-frame scroll counter starting at 0x27F; exact per-image duration not confirmed]". **Q: Accept
   4000ms, or RE the original scroll-counter timing at ScreenExtrasGallery 0x417D50?** (RE-RESOLVABLE
   in principle; USER to accept if exact timing is deemed cosmetic.)

8. **RaceResults / NameEntry / CupFailed / CupWon / SessionLocked sub-state animation timings.**
   Many of these use bare `s_anim_tick += 2; if (>= 0x10/0x12/0x20/16)` frame counters rather than
   the `frontend_update_timed_animation(max, ms)` wall-clock path other screens use. **Q: Is the
   frame-counter vs wall-clock split intentional, and are the 0x10/0x12/0x20 thresholds the original's
   actual counter targets?** (See TIMING rows; mostly RE-RESOLVABLE per-state.)

9. **RaceResults panel alpha 0xE0 (semi-transparent) vs orig opaque.** `td5_frontend.c:4914-4915`
   "[UNCERTAIN] original uses opaque BltColorFillToSurface(0,0,0); port uses 0xE0 so gallery bg stays
   dimly visible." Actually the panel body is NOT drawn (color-key invisible) per :4949. **Q: Confirm
   the panel should be fully invisible (color-key) — i.e. the 0xE0 note is stale and no body quad is
   correct?** (Likely already-resolved by the :4949 logic; flagged to confirm.)

10. **View Replay / View Race Data greying (Screen 24).** `td5_frontend.c:10004-10009` leaves these
    buttons live because the port has no replay-availability flag; orig greys them when
    `g_replayFileAvailable==0`. **Q: Leave live (no-op), grey always, or wire a replay-availability
    flag?** (USER — depends on whether a replay subsystem is in scope.)

11. **Controller-binding joystick path uses keyboard-key surrogate (Screen 18).** `td5_frontend.c:
    8353-8375` "[INFERRED]" maps gamepad buttons to Enter/Backspace/Space/arrows because no live
    DXInput::GetJS frontend API. num_buttons hardcoded to 8 (`:8282-8283 [INFERRED]`). **Q: Accept
    the keyboard surrogate + 8-button assumption, or wire the real joystick poll
    (td5_input GetJS-equivalent) into the binding screen?** (Per MEMORY, joystick pipeline now
    exists — may be wireable.)

12. **ControlOptions device-name panel + skip-empty device walk deferred (Screen 14).**
    `td5_frontend.c:8840-8848` notes the device-NAME panel and skip-empty/skip-equal device walk are
    only "partially mirrored." **Q: Complete the device-name display + walk now, or defer (intersects
    the user-verified keyboard-rebind flow)?**

---

## ASSUMPTIONS BY SCREEN

Legend: cat = POSITION/SIZE/COLOR/TIMING/STRING-LABEL/INDEX-MAP/BEHAVIOR/CONTRADICTION/OTHER.
res = RE-RESOLVABLE / USER-DECISION / ALREADY-CONFIRMED-OK.

### Shared / helpers / module-level

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 159-168 | INDEX-MAP | `k_music_test_band[12]` / `k_music_test_title[12]` band+title tables | RE-RESOLVABLE | Dump PTR_s_GRAVITY_KILLS @0x465e1c (band) + PTR_s_FALLING @0x465e58 (title), 12 ptrs each; confirm strings byte-for-byte. (Marked CONFIRMED but values are decoded-from-binary, re-verify.) |
| 185 | INDEX-MAP | `k_music_track_to_band[12]={1,3,4,4,2,0,0,1,3,4,4,4}` | RE-RESOLVABLE | See C2. Dump LUT @0x465e4c. |
| 345-354 | INDEX-MAP | `s_cup_schedules` row [5] Ultimate = "Placeholder = Masters order" | RE-RESOLVABLE | RE the Ultimate jump-table callback @0x4110A0 to get the real 12-track Ultimate schedule (comment admits placeholder). |
| 362-367 | COLOR | `s_bar_fade_table` bar colors {0x20,0x20,0x40},{0x40,0x10,0x10} + fade types | RE-RESOLVABLE | Read InitFrontendFadeColor @0x411750 + the bar-fade callers; confirm the per-slot RGB + fade_type. Currently unannotated literals. |
| 453-464 | STRING-LABEL | `k_ctrl_action_labels[10]` LEFT..REAR VIEW | ALREADY-CONFIRMED-OK | Matches SNK_ControlText[0..9] exactly (verified vs frontend_snk_strings.md). |
| 469-479 | STRING-LABEL | `k_js_value_labels[9]` "Axis+/Btn1.." placeholders | RE-RESOLVABLE / USER | See USER-DECISION #4. Orig uses SNK_ControlText[value*16] (action names), NOT axis/btn labels. |
| 548-557 | SIZE | `k_font_glyph_advance_default[96]` per-glyph advance widths | RE-RESOLVABLE | Read g_smallFontAdvance/PTR_DAT_004660c8 (BodyText advance LUT) — these 96 bytes should match the original advance table; currently no @addr basis. |
| 562-566 | SIZE | FONT_CELL=24, FONT_COLS=10, FONT_TEX_W=240, FONT_TEX_H=552 | ALREADY-CONFIRMED-OK | From FUN_00424560 (BodyText.tga 240x552, 10/row, 24x24). |
| 607-645 | INDEX-MAP | `s_car_zip_paths[37]` order + lock annotations | ALREADY-CONFIRMED-OK | "UI order matches 0x00463e24". Order confirmed; per-car locked/unlocked comments are advisory. |
| 647-693 | STRING-LABEL/INDEX-MAP | `s_track_display_names[26]` + schedule→name + schedule→tga LUTs | ALREADY-CONFIRMED-OK | Names cross-ref SNK_TrackNames; LUTs from DAT_00466894. Note slots 21-26 placeholder "TRACK 21".."TRACK 26" (only 20 real tracks) — fine. |
| 723 | TIMING | `frontend_update_timed_animation` uses `*2.0f/duration_ms` wall-clock | USER-DECISION | ARCH-DIV: orig is per-frame counter (30fps), port is wall-clock. The `*2.0` and ms durations are derived, not from orig. See USER-DECISION #8. |
| 1226-1229 | POSITION | FE_BTN_LEFT_OFFSET=0xD2 (110); `s_auto_button_y_offset[]` -0x93..0xAD step 0x28 | ALREADY-CONFIRMED-OK | From 0x415490 decomp (documented). But see USER #5 (MainMenu overrides to 0xC2=126). |
| 1351-1357 | SIZE | button width fallback `w != 200 → use w; else 224`; `h != 32 → use h; else 32` | RE-RESOLVABLE | The `!= 200` / `!= 32` sentinel hack is a port heuristic; verify each screen passes the true w/h from its screen fn (the immediates). |
| 1416-1417 | OTHER | `frontend_cd_play(track)` → `td5_plat_cd_play(track + 2)` | ALREADY-CONFIRMED-OK | Matches CDPlay(idx+2) @0x41864E. |
| 3307 / 5156 (button cache 156) | COLOR | button interior fill `0xFF392152` (dark purple) | RE-RESOLVABLE | Read DrawFrontendButtonBackground @0x425b60 / the state-0 fill source; the RGB(0x52,0x21,0x39) is unannotated. |
| 5278-5298 | SIZE/POSITION | ButtonBits 9-slice geometry: 56x100, LW=26, RW=28, corners tl=13/tr=9/bl=9/br=13, tile=4 | ALREADY-CONFIRMED-OK | From FUN_00425b60 (documented, verified twice incl. button cache). |
| 5743-5747 | COLOR | button bg ramp lerp 0x99→0xCC alpha, 0x28→0x50 R, 0x38→0x80 G, 0x58→0xC0 B | RE-RESOLVABLE | These are the FALLBACK colors when ButtonBits unloaded; orig uses surface swap not color lerp (port admits Phase-3 INFERRED @5755-5762). Verify against the baked surface. |
| 5833-5840 | COLOR/POSITION | green hover border 0xFF008000, insets inL=20/inR=22/inT=4/inB=6, 2px bars | RE-RESOLVABLE | Read RenderFrontendDisplayModeHighlight @0x4263e0; confirm the 0xC000-green and the exact inset rects (port comment says "0xc000 edge bars" elsewhere @screens_00.md but uses 0x008000 here). |
| 5928 | POSITION | title strip x = 120 (screenW/2 - 200) | ALREADY-CONFIRMED-OK | Documented across multiple screen fns (uVar4-200). |
| 3939-3940 | POSITION/TIMING | title slide base_y=21, hidden_y=-135, +4px/frame 39 frames | USER-DECISION | Frida-derived (re/tools/frida_main_menu_capture.log), not screen-fn immediates. See USER #6. |
| 3716-3718 | POSITION | FE_VALUE_PANEL_X=394, W=224, CENTER_X=506 | ALREADY-CONFIRMED-OK | From 0x41FF5B (ADD ESI,0x11C; panel 0xE0). |

### Screen 0 — LocalizationInit (0x4269D0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6150-6152 | CONTRADICTION | SNK_LangDLL[8] 0x31=English etc. | RE-RESOLVABLE | **C1** — see CONTRADICTIONS. Read 0x4269DF–0x426A07 + Language.dll SNK_LangDLL bytes. |
| 6162 | BEHAVIOR | "[INFERRED] Enumerate display modes (handled in td5_render.c)" | ALREADY-CONFIRMED-OK | Display-mode enumeration is in BuildEnumeratedDisplayModeList @0x40B100 (delegated, fine). |

### Screen 1 — PositionerDebugTool (0x415030)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6192-6193 | POSITION/STRING-LABEL | "Save"(120,400,96,32) / "Back"(232,400,112,32) | USER-DECISION | Dev-only debug tool, unreachable in shipped build. Orig has NO exit-to-menu button (screens_00.md:112). Port invented Save/Back. ARCH-DIV (dev affordance) — confirm leave-as-is. |

### Screen 3 — LanguageSelect (0x427290)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6310-6313 | STRING-LABEL/POSITION | "English/French/German/Spanish" text buttons at (120,180..300,180,32) | RE-RESOLVABLE | Orig draws 4 FLAG-SHEET image buttons (Language.tga rows, 176x128 at computed corners), NOT text buttons; the choice is non-committal (screens_00.md Screen 3). Read 0x427290 case 0 for the 4 CreateFrontendMenuRectEntry coords + flag-sheet src-y 0/0x80/0x100/0x180. Port's text buttons + positions are invented. |
| 6332-6344 | BEHAVIOR | language choice stored to s_flow_context, advances to Legal | ALREADY-CONFIRMED-OK | Orig also non-committal (no locale write); behavior matches (screens_00.md "KEY FINDING"). |

### Screen 4 — LegalCopyright (0x4274A0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 5424-5430 | STRING-LABEL/POSITION/SIZE | "TEST DRIVE 5 COPYRIGHT 1998", x=64, step 32, 14 rows | ALREADY-CONFIRMED-OK | String @0x466808, x=W/10, y step 0x20, rows (H-0x20)>>5 — all confirmed. |
| 6383 | TIMING | 3-second dwell `> 2999u` | ALREADY-CONFIRMED-OK | Confirmed @0x4274A0 case 2. |

### Screen 5 — MainMenu (0x415490)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6438-6444 | STRING-LABEL | "Race Menu/Quick Race/Two Player/Net Play/Options/High Scores/Exit" | RE-RESOLVABLE | SNK_RaceMenuButTxt="RACE MENU", SNK_QuickRaceButTxt="QUICK RACE", SNK_TwoPlayerButTxt="TWO PLAYER", SNK_NetPlayButTxt="NET PLAY", SNK_OptionsButTxt="OPTIONS", SNK_HiScoreButTxt="HIGH SCORES", SNK_ExitButTxt="EXIT". Case mismatch. See USER #1. |
| 6422 | BEHAVIOR | circuit_lap_count = laps + 1 seed | ALREADY-CONFIRMED-OK | Confirmed @0x004155DE. |
| 6508-6526 | BEHAVIOR | button 2 = TwoPlayer always (app+0x170 never written) | ALREADY-CONFIRMED-OK | xref scan confirms app+0x170 has no writer; benchmark path is port INI affordance (documented). |
| 6572-6573 | STRING-LABEL/POSITION | "Yes"/"No" confirm at exit_x, +100, y+h+8, 96x32 | RE-RESOLVABLE | SNK_YesButTxt="YES"/SNK_NoxButTxt="NO!" (note "NO!" with bang). Confirm the Yes/No dialog coords @0x415490 exit sub-flow (states 5-7). Positions derived from exit button, not confirmed. |
| 6601 | BEHAVIOR | Exit → EXTRAS_GALLERY (credits) then quit | RE-RESOLVABLE | Confirm orig Exit→credits flow vs direct quit @0x415490 state 7. |
| 6621 / 6633 | TIMING | slide-out 16 ticks / 267ms | RE-RESOLVABLE | Confirm orig slide-out frame count @0x4155DE (port uses wall-clock). |

### Screen 6 — RaceTypeCategory (0x4168B0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6690-6700 | STRING-LABEL | "Single Race/Cup Race/Continue Cup/Time Trials/Drag Race/Cop Chase/Back" | RE-RESOLVABLE | SNK_SingleRaceButTxt="SINGLE RACE", SNK_CupRaceButTxt="CUP RACE", SNK_ContCupButTxt="CONTINUE CUP", SNK_TimeTrialsButTxt="TIME TRIALS", SNK_DragRaceButTxt="DRAG RACE", SNK_CopChaseButTxt="COP CHASE", SNK_BackButTxt="BACK". Case mismatch. |
| 6788-6815 | STRING-LABEL | "Championship/Era/Challenge/Pitbull/Masters/Ultimate/Back" | RE-RESOLVABLE | SNK_ChampionshipButTxt, SNK_EraButTxt, SNK_ChallengeButTxt, SNK_PitbullButTxt, SNK_MastersButTxt, SNK_UltimateButTxt, SNK_BackButTxt (all ALL-CAPS). Case mismatch. |
| 4255-4280 | STRING-LABEL | `k_race_desc[12][13]` reconstructed multi-line descriptions | USER-DECISION | See USER #3 — author admits reconstructed after SNK truncation. Re-extract true SNK_RaceTypeText multi-line entries from Language.dll. |
| 4284-4285 | INDEX-MAP | `k_top_to_idx[7]={0,10,11,9,7,8,-1}` / `k_cup_to_idx[7]={1,2,3,4,5,6,-1}` | ALREADY-CONFIRMED-OK | Confirmed @0x416e45/0x4171xx button→gameType map. |
| 4287-4289 | POSITION/SIZE | desc panel x=358 (cx+0x26), y=145 (cy-0x5f), w=272 (0x110) | RE-RESOLVABLE | Confirm the 0x110x0xB4 panel placement @0x4168B0 (port derives 358/145; spec says 0x110x0xB4). |
| 4300-4303 | POSITION/TIMING | desc lines Y=32 step+12 stop>=176, big font line0 | RE-RESOLVABLE | Confirm small-font 12px line step + Y bounds in 0x4168B0 desc draw loop. |

### Screen 7 — QuickRaceMenu (0x4213D0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 6927-6932 | STRING-LABEL | "Change Car/Change Track/OK/Back" | RE-RESOLVABLE | SNK_ChangeCarButTxt="CHANGE CAR", SNK_ChangeTrackButTxt="CHANGE TRACK", SNK_OkButTxt="OK", SNK_BackButTxt="BACK". Case mismatch. |
| 6921-6925 | POSITION/SIZE | ChangeCar(120,137,256,32), ChangeTrack(120,257), OK(120,377,96), Back(232,377,112) | ALREADY-CONFIRMED-OK | "From Ghidra: halfW=320,halfH=240..." documented coords. |
| 3974-3977 | POSITION/COLOR | car/track value at (140,106)/(140,226); LOCKED red 0xFFFF4444 at (398,126)/(398,246) | RE-RESOLVABLE | Confirm value positions + that LOCKED is white (per :4544/4606 fix elsewhere LOCKED→white), here still RED 0xFFFF4444 — possible inconsistency vs the 2026-06-01 "LOCKED renders white" fix. |
| 6962-6974 | BEHAVIOR | car_max 32/36; track_max 0x13/total; wrap | RE-RESOLVABLE | Confirm roster bounds @0x4213D0 (network=36 etc.). |

### Screen 8 — ConnectionBrowser (0x418D50)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7037-7039 | STRING-LABEL | "Provider/OK/Back" at (120,160,256,32)/(-100)/(-100) | USER-DECISION | DXPTYPE ARCH-DIV (no enumerable peers). "Provider" is a port label, not an SNK_ string (SNK_ChooseConnectionButTxt="CHOOSE CONNECTION"). Static 2-button list is ARCH-DIV. Confirm OK/Back use SNK_OkButTxt/SNK_BackButTxt. |

### Screen 9 — SessionPicker (0x419CF0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7120-7122 | STRING-LABEL | "Session/OK/Back" | USER-DECISION | DXPTYPE ARCH-DIV. "Session" port label (SNK_ChooseSessionButTxt="CHOOSE SESSION"). Button-set now faithful (FIXED 2026-06-01 removed spurious Create). Confirm labels. |

### Screen 10 — CreateSession (0x41A7B0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7190-7193 | STRING-LABEL | "Enter Name"/"Back"; default name "New Session" | RE-RESOLVABLE/USER | SNK_EnterNewSessionNameButTxt="ENTER NEW SESSION NAME", SNK_NewSessionTxt="< New Session >" (note brackets). DXPTYPE ARCH-DIV for the flow; labels RE-RESOLVABLE. |

### Screen 11 — NetworkLobby (0x41C330)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7280-7285 | STRING-LABEL/POSITION/SIZE | ""/Messages/Status/Change Car/Start/Exit + widths 0x1D0/0x200/0xE0/200/0x78 | USER-DECISION | DXPTYPE ARCH-DIV (lobby can't complete a session). SNK_MessageWindowButTxt="MESSAGE WINDOW", SNK_StatusButTxt="STATUS", SNK_ChangeCarButTxt="CHANGE CAR", SNK_StartButTxt="START", SNK_ExitButTxt="EXIT". Labels RE-RESOLVABLE; the FSM is structural-only. |
| 7423-7426 | STRING-LABEL | "Yes"/"No"/"Ok" dialog | RE-RESOLVABLE | SNK_YesButTxt/SNK_NoxButTxt="NO!"/SNK_OkButTxt="OK". |
| 7514/7553/7577 | TIMING | poll intervals 250ms / 165ms / 8-tick countdown | RE-RESOLVABLE | Confirm orig DirectPlay poll intervals @0x41C330 (ARCH-DIV but values cited without @addr). |

### Screen 12 — OptionsHub (0x41D890)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7614-7619 | STRING-LABEL | "Game Options/Control Options/Sound Options/Graphics Options/Two Player Options/OK" | RE-RESOLVABLE | SNK_GameOptionsButTxt, SNK_ControlOptionsButTxt, SNK_SoundOptionsButTxt, SNK_GraphicsOptionsButTxt, SNK_TwoPlayerOptionsButTxt, SNK_OkButTxt. Case mismatch. |
| 7651-7665 | BEHAVIOR | OK does NOT commit option shadows (deferred to ConfigureGameTypeFlags) | ALREADY-CONFIRMED-OK | Documented deferred model (audit 2026-05-30); orig commits at 0x41dc8e etc. Intentional divergence, parity-equivalent. |

### Screen 13 — GameOptions (0x41F990)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7704-7711 | STRING-LABEL | "Circuit Laps/Checkpoint Timers/Traffic/Police/Difficulty/Dynamics/3D Collisions/OK" | RE-RESOLVABLE | SNK_CircuitLapsTxt="CIRCUIT LAPS", SNK_CheckpointTimersButTxt, SNK_TrafficButTxt="TRAFFIC", SNK_CopsButTxt="POLICE", SNK_DifficultyButTxt, SNK_DynamicsButTxt, SNK_3dCollisionsButTxt="3D COLLISIONS", SNK_OkButTxt. Widths 0x128/0x60 ALREADY-CONFIRMED (annotated @0x41fa1d/0x41fae3). |
| 4005-4010 | STRING-LABEL | on_off {"OFF","ON"}, difficulty {"EASY","NORMAL","HARD"}, dynamics {"ARCADE","SIMULATION"} | ALREADY-CONFIRMED-OK | Match SNK_OnOffTxt, SNK_DifficultyTxt, SNK_DynamicsTxt exactly. |
| 7746-7747 | BEHAVIOR | laps CLAMP [0,3] (not wrap) | ALREADY-CONFIRMED-OK | Confirmed @0x0041F990 case6/idx0. |
| 4018 | BEHAVIOR | laps displayed = idx+1, no *2 | ALREADY-CONFIRMED-OK | Confirmed @0x0041FD78. |

### Screen 14 — ControlOptions (0x41DF20)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7810-7814 | STRING-LABEL | "Player 1/Configure/Player 2/Configure/OK" | RE-RESOLVABLE | SNK_Player1ButTxt="PLAYER 1", SNK_ConfigureButTxt="CONFIGURE", SNK_Player2ButTxt="PLAYER 2", SNK_OkButTxt. Also SNK_Ctrl_Modes={"KEYBOARD","JOYSTICK","JOYPAD","WHEEL","<NONE>"} for the device names. Case mismatch. |
| 8840-8848 | BEHAVIOR | device-name panel + skip-empty/skip-equal walk deferred | USER-DECISION | See USER #12 (fix_10.md S14). |
| 4615-4635 | POSITION | Controllers.tga icons P1(394,97)/P2(394,217), row=type*32 | ALREADY-CONFIRMED-OK | From FUN_0041DF20 case4/5 (documented). |

### Screen 15 — SoundOptions (0x41EA90)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 7907-7911 | STRING-LABEL | "SFX Mode/SFX Volume/Music Volume/Music Test/OK" | RE-RESOLVABLE | SNK_SfxModeButTxt="SFX MODE", SNK_SfxVolumeButTxt, SNK_MusicVolumeButTxt, SNK_MusicTestButTxt, SNK_OkButTxt. Widths 0x100/0x60 ALREADY-CONFIRMED. |
| 4082 | STRING-LABEL | sfx_mode_names {"MONAURAL","STEREO","3D SOUND"} | ALREADY-CONFIRMED-OK | Match SNK_SFX_Modes exactly. |
| 7940-7957 | BEHAVIOR | 3-mode SFX cycle unconditional; volume step *10 | ALREADY-CONFIRMED-OK | Confirmed @0x0041F2EB (REG-2 fix). |
| 4052-4057 | POSITION | volume box (394,185)/(394,225) 224x12, fill 222x10 | RE-RESOLVABLE | Comment cites "FUN_0041EA90" but no addr for the box/fill Y; confirm 185/225 + 222px fill. |

### Screen 16 — DisplayOptions (0x420400)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 8002-8012 | STRING-LABEL | "Resolution/Fogging/Speed Readout/Camera Damping/OK" | RE-RESOLVABLE | SNK_ResolutionButTxt, SNK_FoggingButTxt, SNK_SpeedReadoutButTxt="SPEED READOUT", SNK_CameraDampingButTxt, SNK_OkButTxt. Widths 0x120/0x60 ALREADY-CONFIRMED. |
| 4031-4032 | STRING-LABEL | on_off {"OFF","ON"}, speed_read {"MPH","KPH"} | ALREADY-CONFIRMED-OK | Match SNK_OnOffTxt, SNK_SpeedReadTxt. |
| 4030 | STRING-LABEL | mode_name fallback "UNAVAILABLE" | RE-RESOLVABLE/USER | Port string when no display modes; orig uses the enumerated mode string. Confirm no orig "UNAVAILABLE" label exists. |
| 8003-8008 | BEHAVIOR | Fogging always-live (CanFog()==1 under D3D11) | ALREADY-CONFIRMED-OK | Documented parity reasoning (audit 2026-05-30). |
| 8063 | BEHAVIOR | camera damping clamp 0..9 | RE-RESOLVABLE | Confirm clamp range @0x420400 case 6. |

### Screen 17 — TwoPlayerOptions (0x420C70)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 8100-8104 | STRING-LABEL | "Split Screen/Catchup/OK" | RE-RESOLVABLE | SNK_SplitScreenButTxt="SPLIT SCREEN", SNK_CatchupTxt="CATCHUP", SNK_OkButTxt. Positions ALREADY-CONFIRMED (@0x420d22/d33/d43). |
| 4216 | STRING-LABEL | split_modes {"LEFT/RIGHT","UP/DOWN"} | ALREADY-CONFIRMED-OK | Match SNK_Split_Modes. |
| 4226-4227 | POSITION | SplitScreen.tga icon at (394,97) 64x32 | ALREADY-CONFIRMED-OK | Confirmed @0x4210A4. |

### Screen 18 — ControllerBinding (0x40FE00)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 8282-8286 | BEHAVIOR | num_buttons hardcoded 8 ("[INFERRED] typical gamepad") | USER-DECISION/RE | Orig reads button count from device descriptor (DAT_00490ba4, clamp <4→2, >=9→8). Port can't query → assumes 8. See USER #11. |
| 8353-8375 | BEHAVIOR | joystick bits = keyboard surrogate (Enter/Backspace/Space/arrows → bits 18-25) "[INFERRED]" | USER-DECISION | See USER #11. No live GetJS in frontend. |
| 4673-4682 | POSITION | binding list action label x=160 on 448x216, surf origin canvas (96,130) "[UNCERTAIN] iVar13=0 assumed" | RE-RESOLVABLE | Read 0x40FE00 case 10: confirm uStack_14 = iVar13 + num_buttons*-12 + 0x9a and the canvas blit origin (port assumed iVar13=0 + origin 96,130). |
| 4709-4711 | POSITION | keyboard prompt label y=0x18 on 448x64; "[UNCERTAIN] origin canvas y=130" | RE-RESOLVABLE | Read 0x40FE00 case 0x19: confirm the header-surface canvas Y. |
| 4719-4732 | STRING-LABEL/POSITION | "PRESS KEY FOR:" + "%d / 10" progress, custom layout | RE-RESOLVABLE | Orig uses SNK_PressKeyTxt="PRESS THE KEY TO USE FOR" + SNK_ControlText label, NOT "PRESS KEY FOR:". Port abbreviated. Also progress "%d / 10" is a port invention. |
| 8284-8302 | BEHAVIOR | axis-validation defaults (slots 1/2 reset to 4/5) | ALREADY-CONFIRMED-OK | Confirmed @0x40FE00. |

### Screen 19 — MusicTestExtras (0x418460)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 8612-8613 | STRING-LABEL/POSITION | "TRACK"(120,97,0xA0)/"OK"(216,377,0x60) | ALREADY-CONFIRMED-OK | SNK_SelectTrackButTxt="TRACK"; positions confirmed @0x418460 (FIXED from "Select Track" guess). |
| 8617-8620 | STRING-LABEL | cover filenames "Fear Factory.tga" etc. | ALREADY-CONFIRMED-OK | Confirmed load order @0x40d6a0. |
| 8675-8676 | BEHAVIOR | track index WRAPS 0<->11 (not clamp) | ALREADY-CONFIRMED-OK | Confirmed @0x00418460 case6 (re-decompiled 2x). |
| 4180 | POSITION | album cover at (118,140) native size | ALREADY-CONFIRMED-OK | Confirmed @0x40d830/0x40d190. |
| 4194/4201-4207 | POSITION/STRING-LABEL | track label centered x=454; now-playing rows y0/0x28/0x50; "NOW PLAYING:" | ALREADY-CONFIRMED-OK | Confirmed @0x418527; SNK_NowPlayingTxt="NOW PLAYING:". |

### Screen 20 — CarSelection (0x40DFC0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 8868-8875 | STRING-LABEL | "Car/Paint/Stats/Automatic|Manual/OK/Back" | RE-RESOLVABLE | SNK_CarButTxt="CAR", SNK_PaintButTxt="PAINT", SNK_ConfigButTxt="STATS", SNK_AutoButTxt="AUTOMATIC"/SNK_ManualButTxt="MANUAL", SNK_OkButTxt, SNK_BackButTxt. Case mismatch. (Positions 46,169.. ALREADY-CONFIRMED @0x40DFC0 state 4.) |
| 4544-4552 | STRING-LABEL/POSITION/COLOR | "LOCKED" white at (86,163); "BEAUTY"/"BEAST" tag | ALREADY-CONFIRMED-OK | SNK_LockedTxt="LOCKED", SNK_BeautyTxt="BEAUTY", SNK_BeastTxt="BEAST"; pos confirmed @0x40DFC0 case0xc (FIXED 2026-06-01). |
| 8782-8814 | BEHAVIOR/INDEX-MAP | roster ranges per game type (Era 0-15, Cop 33-36, default cap 32) | ALREADY-CONFIRMED-OK | Confirmed @0x0040E8F8 (DAT_004962ac gate documented). |
| 4313-4325 | STRING-LABEL | k_stat_layout_types[6] / k_stat_engine_types[19] | ALREADY-CONFIRMED-OK | Match SNK_Layout_Types / SNK_Engine_Types exactly. |
| 4370-4385 | STRING-LABEL | k_rows hdr labels "LAYOUT:".."HP:" + sfx " MPH"/" sec"/" ft" | RE-RESOLVABLE | Headers match SNK_Config_Hdrs. BUT sfx suffixes are lowercase port strings (" sec"," ft"); orig SNK_ConfSpeed/Mph/Sec/Ft are " SPEED"/" MPH"/" SEC"/" FT" (ALL-CAPS). Confirm + match case. |
| 9073-9078 | BEHAVIOR | Info sub-screen (state 17) falls through (Language.dll SNK_Info_Values unavailable) | USER-DECISION | Orig draws 10 SNK_Info_Values lines (modification notes). Port skips. See USER #2 (Language.dll). |
| 9056-9060 | TIMING | car slide-in 25 frames/833ms; slide-out 0x18/400ms | ALREADY-CONFIRMED-OK | Confirmed @0x0040DF4A / case 0xE. |

### Screen 21 — TrackSelection (0x427630)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 9319-9324 | STRING-LABEL | "Track/Forwards/OK/Back" | RE-RESOLVABLE | SNK_TrackButTxt="TRACK", SNK_ForwardsButTxt="FORWARDS"/SNK_BackwardsButTxt="BACKWARDS", SNK_OkButTxt, SNK_BackButTxt. Case mismatch. (Positions 120,97.. documented.) |
| 9402-9404 | STRING-LABEL | "Backwards"/"Forwards" toggle label | RE-RESOLVABLE | SNK_BackwardsButTxt/SNK_ForwardsButTxt (ALL-CAPS). |
| 4582-4593 | POSITION | track name centered x=492; preview at (412,135) 152x224 | RE-RESOLVABLE | Comment derives 492 (344+148) and 412/135 from FUN_00424a50 / EDI+0x12E; confirm against 0x427630 immediates. |
| 4564-4565 | TIMING | preview slide x=(16-tick)*16, text y=(tick-16)*16, 16 frames | ALREADY-CONFIRMED-OK | Confirmed @0x427b80/0x427957/0x427e96. |
| 9308-9314 | BEHAVIOR | track_max network=18 else total_unlocked | RE-RESOLVABLE | Confirm bounds @0x427630 case 0. |
| 9276-9290 | BEHAVIOR | Direction toggle hidden on forward-only tracks (gate on reverse-data presence) | ALREADY-CONFIRMED-OK | Documented divergence (orig gates on 0x4A2C98 unlock byte; port gates on reverse-data; equivalent). |

### Screen 22 — ExtrasGallery (0x417D50)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 9496-9503 | INDEX-MAP/STRING-LABEL | `s_gallery_names[27]` (Legals5..1, then 22 mugshots) | ALREADY-CONFIRMED-OK | "Original push order from 0x465AAC string table." |
| 9490 | OTHER | GALLERY_PIC_COUNT=27 | ALREADY-CONFIRMED-OK | Matches orig 27 surfaces from Mugshots.zip. |
| 9533-9535 | TIMING | 4000ms per-image dwell "[UNCERTAIN]" | USER-DECISION/RE | See USER #7. Orig per-frame scroll counter @0x27F; exact duration not confirmed. |

### Screen 23 — PostRaceHighScore (0x413580)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 9578-9579 | STRING-LABEL/POSITION | NULL nav bar (115,93,520,32); "OK"(120,416,96,32) | RE-RESOLVABLE | SNK_OkButTxt. Confirm OK y=416 (others use 377) @0x413580. |
| 4778-4799 | POSITION/SIZE | panel (115,177) 520x144; column X name+16/score+128/car+228/avg+352/top+444 | RE-RESOLVABLE | Panel 0x208x0x90 confirmed; but the 5 column X offsets (16/128/228/352/444) are port-derived — confirm against DrawPostRaceHighScoreEntry @0x413010 column layout. |
| 4789-4792 | COLOR | panel backdrop white border + 0xF0101018 dark fill | RE-RESOLVABLE | Orig black-fills the 0x208x0x90 surface (FUN_00424050(0,0,0,...)); port adds a 0xF0101018 + white border. Confirm orig fill color (port "visual parity" addition). |
| 4820-4831 | STRING-LABEL | headers NAME/BEST/TIME|LAP|POINTS/CAR/AVERAGE/TOP + "SPEED" | ALREADY-CONFIRMED-OK | From SNK_ strings (NAME, BEST, TIME, LAP, POINTS, CAR, AVERAGE, TOP, SPEED) — FIXED 2026-06-01 from prior abbreviation guesses. |
| 4805/4851 | STRING-LABEL | "NO SCORES YET" / "---" empty markers | RE-RESOLVABLE/USER | Port-invented empty-state strings; orig may draw blank rows. Confirm no SNK_ equivalent. |
| 4846 | COLOR | inserted-row highlight gold 0xFFFFCC44 | RE-RESOLVABLE | Orig bolds via SmallTextb bold atlas; port uses gold color. Confirm (no bold atlas in port) — ARCH-DIV color substitution. |
| 4747/4765-4767 | OTHER | tick→time (raw*100/30) + speed convert (raw*256+389)/778 KPH / +625/1252 MPH | ALREADY-CONFIRMED-OK | Same conversion as td5_hud.c (30fps tick base). |

### Screen 24 — RaceResults (0x422480)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 9997-10001 | STRING-LABEL | "Next Cup Race"/"Race Again"/"Save Race Status"/"Select New Car"/"Quit"/"OK" + "View Replay"/"View Race Data" | RE-RESOLVABLE | SNK_NextCupRace="NEXT CUP RACE", SNK_RaceAgain="RACE AGAIN", SNK_SaveRaceStatus="SAVE RACE STATUS", SNK_SelectNewCar="SELECT NEW CAR", SNK_Quit="QUIT", SNK_ViewReplay="VIEW REPLAY", SNK_ViewRaceData="VIEW RACE DATA", SNK_OkButTxt. Case mismatch. |
| 9979-9986 | POSITION/SIZE | menu buttons 288x32, Y -0x8F/-0x5F/-0x2F/+1/+0x31 step 0x30 | ALREADY-CONFIRMED-OK | Confirmed @0x004231D6-0x0042323C. |
| 4938-4940 | POSITION/SIZE | panel 408x392 at (152,81) | ALREADY-CONFIRMED-OK | Confirmed @DrawRaceDataSummaryPanel 0x421e90 + case 0. |
| 5014-5044 | STRING-LABEL/INDEX-MAP | k_results_labels[] + per-game-type row ladders | ALREADY-CONFIRMED-OK | From SNK_ResultsTxt/DRResultsTxt/CCResultsTxt (extracted verbatim). |
| 4914-4917 | COLOR/BEHAVIOR | panel alpha 0xE0 "[UNCERTAIN]" + simplified columns "[UNCERTAIN]" | USER-DECISION | See USER #9. Note :4949 says no body draw (color-key) — the 0xE0/columns notes are stale; confirm. |
| 5099-5105 | BEHAVIOR | CUP_POSITION = finish_position+1 "coarse fallback" | RE-RESOLVABLE | Orig reads cup-position LUT (&DAT_004660b4)[slot+0x14*0x14+0x2]. Port uses finish_position. RE the LUT to surface true cup position. |
| 5159-5167 | BEHAVIOR | CHECKPOINT_TIMERS = single best-lap value (orig lists all splits) | USER-DECISION/RE | Orig loops short[] at slot+0x34e (all lap splits). Port emits one summary line. Confirm intent. |
| 10004-10009 | BEHAVIOR | View Replay/Race Data left live (no replay subsystem) | USER-DECISION | See USER #10 (fix_20.md S24). |
| 10144-10155 | BEHAVIOR | Quit 4-branch dispatch (won via finish_position==0) | ALREADY-CONFIRMED-OK | Confirmed @0x004233E0 (P6 fix). |
| 10169-10173 | STRING-LABEL | "Block Saved OK"/"Failed to Save" | RE-RESOLVABLE | SNK_BlockSavedOK="BLOCK SAVED OK", SNK_FailedToSave="SAVE FAILED" (note: NOT "Failed to Save" — orig is "SAVE FAILED"). Case + wording mismatch. |
| 9837-9936 | TIMING/POSITION | panel slide steps 0x20/0x30, 0x11-tick spans | ALREADY-CONFIRMED-OK | Confirmed @0x00422480 cases 7-0xB (formulas cited). |

### Screen 25 — PostRaceNameEntry (0x413BC0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 10311 | STRING-LABEL | default name "PLAYER" | RE-RESOLVABLE/USER | Confirm orig default name buffer seed @0x413BC0 (port assumes "PLAYER"). |
| 10235-10247 | BEHAVIOR/INDEX-MAP | group index: cup=gt+0x13, drag(7)=0x13, else track | ALREADY-CONFIRMED-OK | Confirmed @0x00413BCF. |
| 10367-10407 | BEHAVIOR | insert/shift logic + avg=total/race_count | ALREADY-CONFIRMED-OK | Confirmed @0x00413CB0 case 4. |
| 10319/10336/10442/10464 | TIMING | slide 0x10/0x12/16-tick frame counters | RE-RESOLVABLE | Confirm orig frame targets (port uses bare counters not wall-clock). |

### Screen 26 — CupFailed (0x4237F0)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 5459-5497 | STRING-LABEL/POSITION | "SORRY"/"YOU FAILED"/"TO WIN"/cup name; dialog (152,97) 408x112; y 0/0x1c/0x38/0x54 | ALREADY-CONFIRMED-OK | All from SNK_ strings + confirmed offsets @0x4237F0. |
| 5485 | COLOR | dialog bg 0xCC000000 (semi-transparent black) | RE-RESOLVABLE | Orig BltColorFillToSurface fills opaque black; port uses 0xCC alpha. Confirm orig opacity (port softens it). |
| 5459-5466 | STRING-LABEL | k_cup_type_names[1..6] | ALREADY-CONFIRMED-OK | Match SNK_RaceTypeText cup entries. |
| 10507 | STRING-LABEL | "OK" at -0x120 | ALREADY-CONFIRMED-OK | SNK_OkButTxt; pos confirmed @0x4237F0. |

### Screen 27 — CupWon (0x423A80)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 5541-5559 | STRING-LABEL/POSITION | "CONGRATULATIONS!"/"YOU HAVE WON"/cup; "%d CARS UNLOCKED"/"%d TRACKS UNLOCKED" | ALREADY-CONFIRMED-OK | SNK_CongratsTxt="CONGRATULATIONS!", SNK_YouHaveWonTxt="YOU HAVE WON THE" (note: orig has trailing " THE"; port drops it — minor), SNK_CarsUnlocked="CARS UNLOCKED", SNK_TracksUnlocked="TRACKS UNLOCKED". FIXED 2026-06-01. |
| 5544 | STRING-LABEL | "YOU HAVE WON" (port) vs SNK_YouHaveWonTxt="YOU HAVE WON THE" | RE-RESOLVABLE | Minor: SNK string ends in " THE" (the cup name follows). Confirm orig draws "...WON THE\n[CUP]". |
| 5485/5539 | COLOR | dialog bg 0xCC000000 | RE-RESOLVABLE | Same as Screen 26 — orig opaque black. |
| 10602 | STRING-LABEL | "OK" at -0x120 | ALREADY-CONFIRMED-OK | Pos confirmed @0x00423C61. |

### Screen 28 — StartupInit (0x415370)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 10638 | STRING-LABEL/POSITION | "OK" at (-100,0,100,0x20) | RE-RESOLVABLE | SNK_OkButTxt. Width 100 vs the 0x60 used elsewhere — confirm @0x415370. (Screen is a 4-frame redirect; cosmetic.) |

### Screen 29 — SessionLocked (0x41D630)

| file:line | cat | assumed value | res | what-to-read OR user-question |
|---|---|---|---|---|
| 5586-5588 | STRING-LABEL/POSITION | "SORRY"/"SESSION LOCKED"; dialog (152,97) 408x112; y 0/0x38 | ALREADY-CONFIRMED-OK | SNK_SorryTxt, SNK_SeshLockedTxt; offsets confirmed @0x41D630. |
| 10681 | STRING-LABEL | "OK" at -0x120 | ALREADY-CONFIRMED-OK | Pos confirmed @0x41D630. |
| (whole screen) | BEHAVIOR | reachable only by manually setting s_kicked_flag | USER-DECISION | DXPTYPE ARCH-DIV (no remote peer sets the flag). |

---

## td5_platform_win32.c (frontend-relevant)

| item | cat | note | res |
|---|---|---|---|
| Name-entry input VK 0x30–0x5A + SPACE/BACK/RETURN (per diff_25.md:29, td5_frontend.c:2434 loop) | BEHAVIOR | ARCH-DIV: orig uses DXInput::GetString middleware edit box; port polls GetAsyncKeyState over a VK range. No faithful port possible (no DXInput). | USER-DECISION (documented ARCH-DIV) |
| Frame-dump dev tool | OTHER | Out of scope per task; no frontend constants of concern found. | n/a |

---

## NOTES ON THE SNK DUMP TRUNCATION (multi-line risk)

`frontend_snk_strings.md` truncated multi-line SNK entries at the first NUL. The following port
tables are at RISK of being incomplete vs the true (multi-segment) SNK arrays and MUST be
re-extracted with a NUL-walking dumper before being trusted:
- **`SNK_RaceTypeText`** (Screen 6 `k_race_desc`) — CONFIRMED truncated; port reconstructed the text
  by guess (USER #3). HIGH PRIORITY re-extract.
- **`SNK_Info_Values`** (Screen 20 state 17) — multi-line car-modification notes; port skips entirely.
- **`SNK_CarLongNames`** (matrix[185/5]) — full localized car names (port uses config.eng/nfo instead).
- Any SNK_*_MT / SNK_*_Ex "message-ticker" strings (e.g. SNK_MainMenu_Ex="CLICK TO GO TO OPTIONS
  SCREEN") — the port does NOT render the bottom-of-screen hint ticker at all (a MISSING element,
  not just a label divergence). Confirm whether the hint ticker should be ported.

---
## RESOLVED 2026-06-01 (goal: zero assumptions)

### C1 — SNK_LangDLL[8] English gate byte → SETTLED = 0x30
Byte-verified from Language.dll export SNK_LangDLL = "LANGDLL 0 : ENGLISH/US"; byte[8]='0'=0x30.
Compare immediate 0x30 at 0x00424568 / 0x004242b8 / CreateMenuStringLabelSurface 0x00412e30.
English = the ==0x30 (JZ-taken) localized-blit branch. Port comment claiming 0x31 was WRONG → fixed
(td5_frontend.c ~6150). NOT an assumption anymore.

### Hint ticker (SNK_*_Ex / _MT) → CONFIRMED NOT DRAWN in retail D3D build (TWO independent RE passes)
The inline-string table base 0x00496374 (32 slots) / count 0x004963f4 / x 0x004963f8 / y 0x004963fc /
sentinel 0x00465e10 is WRITE-ONLY. Every reference is a write (setters SetFrontendInlineStringTable
@0x004183b0 / SetFrontendInlineStringEntry @0x00418410); ZERO reads in the EXE, the flush path
(RunFrontendDisplayLoop 0x414b50 / RenderFrontendUiRects 0x425a30 / FlushFrontendSpriteBlits 0x425540 /
callees), or M2DX/M2DXFX (only Print/PrintTGA single-string exports; table never passed to a DLL).
The D3D build imports M2DX not M2DXFX. (NOTE: 0x496370 = base-4 IS read but is a separate
label-width scalar, not the table.) → FAITHFUL BEHAVIOR = the port does NOT draw the hint ticker
(matches retail). Implementing it would be a DEVIATION. Hint strings extracted byte-exact for the
record (see below) but are intentionally NOT rendered.

Full per-screen hint strings (byte-exact from Language.dll, for reference only — NOT drawn):
SNK_MainMenu_Ex="CLICK TO GO TO OPTIONS SCREEN"; SNK_MainMenu_MT=[CLICK TO GO TO RACE MENU / QUICK
RACE MENU / TWO PLAYER RACE / NET PLAY MENU / OPTIONS / EXIT TEST DRIVE 5]; SNK_Options_MT,
SNK_RaceMenu_MT, SNK_RaceMen2_MT, SNK_GameOptions_MT, SNK_GfxOptions_MT, SNK_SfxOptions_MT,
SNK_TwoOptions_MT, SNK_MusicTest_MT, SNK_QuickRaceMenu_MT, SNK_CarSelect_MT1, SNK_TrackSel_MT1,
SNK_CtrlOptions_MT, and _Ex variants — all captured in this session's logs.

### Animation timing model → ACCEPTED ARCH-DIV (user decision 2026-06-01)
The original advances g_frontendAnimFrameCounter (@0x0049522c) ONE TICK PER RENDERED FRAME
(incremented @0x00414eb0), with transitions on exact counter equality (slide-in 0x27 hold@0x26
+ 3-frame AdvanceFrontendTickAndCheckReady gate; slide-out 0x10, or 0x20 on MusicTest/RaceResults;
dialogs 0x20). The port's frontend_update_timed_animation(max_tick, duration_ms) already targets
the CORRECT end thresholds (screens pass 0x27/0x10 as max_tick) but PACES them by wall-clock
duration_ms instead of per-frame. USER DECISION: keep duration-based pacing — the destination tick
is faithful; only the interpolation curve differs (D3D11 vsync/variable-framerate vs the original's
fixed-step loop). NOT an assumption to fix; accepted ARCH-DIV. EXCEPTIONS confirmed genuinely
wall-clock in the ORIGINAL too (so already faithful, leave as ms): Legal 3-second hold (timeGetTime
> 2999 @0x4274a0 case2); MainMenu attract-mode 50000ms idle. Per-screen thresholds are recorded
should a future frame-locked mode be added.

### S3 flag tile-to-corner ORDER → 1 residual minor uncertainty (flagged to user)
CONFIRMED (decomp @0x00427290): 4 CreateFrontendMenuRectEntry @0x004258f0 — TL dest(72,128) srcY=0;
TR dest(392,128) srcY=0x80; BL dest(72,320) srcY=0x100; BR dest(392,320) srcY=0x180. w=176 h=128.
Language.tga descriptor byte @off17 = 0x08 → BOTTOM-LEFT origin. Storage tile order (first bytes→):
purple/red/green/blue. The project's PNG extractor FLIPPED (honored bottom-left) so PNG V=0 tile =
blue, V=3 = purple. Port overlay maps V=0→top-left, so port renders BLUE at TL.
RESIDUAL UNKNOWN: whether the ORIGINAL's TGA loader (LoadFrontendTgaSurfaceFromArchive @0x412030)
also flips on load. If it does → original renders srcY=0 as the visual-bottom (purple) at TL, i.e.
port is currently corner-flipped (should be purple TL / blue BR, swap the V mapping). If the loader
stores verbatim → port's blue-at-TL is correct. Dest rects + srcY are CONFIRMED; only the 1-line
V→corner mapping depends on the loader-flip. LOW IMPACT (near-unreachable boot screen, content is
identical colored boxes either way). DECISION PENDING: decompile 0x412030 flip-handling OR accept.

### S3 tile-order → RESOLVED (loader FLIPS; port already correct)
LoadFrontendTgaSurfaceFromArchive @0x412030 copies verbatim, but the M2DX decoder it calls
(DX::ImageProTGA, M2DX.dll ord 0x5e @0x1000eed0) FLIPS bottom-origin TGAs: at 0x1000ffd0 it seeds
the dest row at total-width and decrements (file row 0 -> last in-memory row). Language.tga is
bottom-origin (desc 0x08), file tiles purple/red/green/blue, so in-memory srcY=0 = BLUE (top tile),
purple = bottom. The project's PNG extractor also flipped (V=0=blue), and the port overlay maps
V=0->top-left -> BLUE at TL. This MATCHES the original (which blits in-memory srcY=0=blue to TL).
S3 tile order is FAITHFUL — no change. CLOSED.
