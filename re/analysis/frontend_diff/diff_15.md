# Frontend PORT-vs-ORIGINAL Diff — Screens 15-19

RESEARCH ONLY. Authoritative gap list for the faithfulness fix pass.
Sources:
- ORIGINAL spec: `re/analysis/frontend_screens/screens_15.md` (Ghidra VAs)
- Engine model: `re/analysis/frontend_rendering_model.md`, `frontend_flow_model.md`
- PORT: `td5mod/src/td5re/td5_frontend.c` (+ `td5_frontend_button_cache.c`)

CLASS legend: MATCH | BUG | DIVERGENCE (intentional/backend-forced) | MISSING | EXTRA | ARCH-DIV.
Cross-cutting tags: `[FONT]` `[DECOUPLED]` `[LABEL]` `[ASSET]` `[ANIM]`.

NOTE on shared infrastructure: all 5 port screens render their value/icon overlays
LIVE every frame in `frontend_render_*_overlay` via `fe_draw_text` / `fe_draw_quad`,
gated on `s_anim_complete`. The original bakes value strings into offscreen DDraw
surfaces (`g_lobbyErrorDialogSurface` etc.) in state 4 and the per-frame FLUSH
(`FlushFrontendSpriteBlits` 0x425540) composites them. This is a global
`[ARCH-DIV]` (the "decoupled FLUSH vs live-render" model) already accepted project-
wide; it is called out once per screen but is not individually a BUG.

================================================================================
## Screen 15 — ScreenSoundOptions (0x0041EA90)
Port: `Screen_SoundOptions` td5_frontend.c:7749 ; overlay
`frontend_render_sound_options_overlay` td5_frontend.c:4025
================================================================================

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background MainMenu.tga + secondary cache | LoadTga16bpp + CopyPrimaryToSecondary (state 0) | :7754 frontend_load_tga | MATCH | — |
| Header "SOUND OPTIONS" | CreateMenuStringLabelSurface(6), animated X | live header render (string-baker fold) | ARCH-DIV `[LABEL]` | accepted fold |
| Row0 "SFX MODE" btn, x=-0x100 w=0x100 +arrows(0,1) | 0x41eb5e | :7762 "SFX Mode" -0x100/0x100 | MATCH | label case only |
| Row1 "SFX VOLUME" +arrows(1,1) | SNK_SfxVolumeButTxt | :7763 "SFX Volume" | MATCH | — |
| Row2 "MUSIC VOLUME" +arrows(2,1) | SNK_MusicVolumeButTxt | :7764 "Music Volume" | MATCH | — |
| Row3 "MUSIC TEST" no arrows, confirm→scr19 | SNK_MusicTestButTxt | :7765 + :7816 | MATCH | — |
| Row4 "OK" x=-0x60 w=0x60, ESC idx=4 | 0x41ebd0, g_frontendEscKeyButtonIndex=4 | :7766 "OK" -0x60/0x60 | MATCH (ESC idx via parent route) | — |
| **SFX-mode icon (3-row)** | QueueOverlayRect src_y=(mode+4)*0x20, 64×32, Controllers.tga@0x466044, states 3..8 | :4043-4056 fe_draw_quad rows (mode+4)/7, Controllers.TGA | MATCH `[ASSET]` (already-fixed per prompt) | confirm: 7-row sheet, modes 0/1/2→rows 4/5/6 ✓ |
| **3-mode vs 2-mode (CanDo3D)** | state6: CanDo3D()==0 → wrap 0/1 (2 modes); else 0/1/2 | :7795-7797 unconditional 0/1/2 wrap | DIVERGENCE (backend always 3D) | accepted (documented :7790) |
| Master-volume bar BG (VolumeBox) | QueueOverlayRect 0xe0×0xc @ (cx+0x4a, cy-0x37), states 3..8 | :4074-4079 fe_draw_quad 224×12 @ (394, btn[1].y+10) | MATCH (live) `[ARCH-DIV]` | — |
| Master-volume bar FILL (VolumeFill) | w=(masterVol%·0xde)/100, 10px, suppress if 0 | :4080-4086 fill_w=vol/100·222, suppress if 0 | MATCH | 0xde=222 ✓ |
| CD/music-volume bar BG + FILL | second VolumeBox/Fill @ cy-0xf / cy-0xe | :4062-4088 btn[2] row | MATCH (live) | — |
| **SFX-mode NAME text box** | dialog 0xe0×0xa0: SNK_SFX_Modes[mode] baked under icon (state4/6) | NOT rendered (:4028 comment "no extra text needed") | MISSING | original shows readable mode NAME ("STEREO"/"MONO"/"SURROUND") under the icon; port shows icon only. Add a centered label under the 64×32 icon at (cx+0x4a≈394, cy-0x8f≈97) area. [UNCERTAIN: SNK_SFX_Modes string contents not byte-read in spec] |
| Volume input step delta·10, clamp[0,100] | state6 idx1/idx2 += dir·10 | :7801/7808 delta·10 clamp | MATCH | — |
| SFX-mode change → SetPlayback(mode) + re-bake | DXSound::SetPlayback | :7795 sets s_sound_option_sfx_mode (no SetPlayback call) | BUG (minor) | port mutates mode but does not call a `td5_sound_set_sfx_mode`/playback API; verify the 3D playback mode actually changes audio routing, not just the icon |
| Volume/mode SFX feedback Play(2) | — | :7814 frontend_play_sfx(2) | MATCH | — |
| Slide-in/out anim | frame-count: in 0x27, out 0x10; bar widths scaled by anim/0x27 | :7775 timed 0x27/650ms; :7830 timed 16/267ms; bars NOT width-scaled during slide | DIVERGENCE `[ANIM]` (ms-timed not frame-count; bars pop instead of grow) | low priority; bar grow-in is cosmetic |

Class counts: MATCH 12, MISSING 1, BUG 1, DIVERGENCE 2, ARCH-DIV (header) 1.

================================================================================
## Screen 16 — ScreenDisplayOptions (0x00420400)
Port: `Screen_DisplayOptions` td5_frontend.c:7845 ; overlay
`frontend_render_display_options_overlay` td5_frontend.c:4007 ;
labels `frontend_refresh_display_option_labels` td5_frontend.c:1938
================================================================================

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background + secondary cache | state0 | :7851 | MATCH | — |
| Header "DISPLAY OPTIONS" | CreateMenuStringLabelSurface(6) | live render | ARCH-DIV `[LABEL]` | accepted fold |
| Row0 "RESOLUTION" x=-0x120 w=0x120 +arrows(0,1) | 0x420484 | :7857 -0x120/0x120 | MATCH | — |
| Row1 "FOGGING" CanFog branch | CanFog()==1 → live cycler+arrows+value; !=1 → DISABLED preview button, NO value | :7864 always live button "Fogging" | DIVERGENCE (backend always fogs) | documented :7858; faithful only because D3D11 CanFog==1 always. No preview/disabled path ported (acceptable). |
| Row2 "SPEED READOUT" +arrows(2,1) | SNK_SpeedReadoutButTxt | :7865 "Speed Readout" | MATCH | — |
| Row3 "CAMERA DAMPING" +arrows(3,1) | SNK_CameraDampingButTxt | :7866 "Camera Damping" | MATCH | — |
| Row4 "OK" x=-0x120 w=0x60, ESC idx=4 | 0x420522 | :7867 -0x60/0x60 | MATCH | — |
| **Resolution value text** | dialog: g_displayModeStringTable[ordinal·0x20] | :4017 s_display_mode_names[idx], else "UNAVAILABLE" | DIVERGENCE `[LABEL]` | label format "%dx%d %dbpp" vs orig "%dx%dx%d" — documented ARCH-DIV (:2062) |
| **Fogging value ON/OFF** | baked only if CanFog==1: *(SNK_OnOffTxt+shadow·4) | :4020 on_off[fog&1] always | MATCH (given CanFog==1) | — |
| **Speed-readout value MPH/KPH** | *(SNK_SpeedReadTxt+shadow·4) | :4021 {"MPH","KPH"}[units&1] | MATCH `[UNCERTAIN]` (string source not byte-read) | — |
| **Camera-damping value (numeric 0..9)** | sprintf via g_uiFormatStringScratchTemplate | :4018 snprintf "%d" | MATCH | template format not byte-read |
| Value dialog panel 0xe0×0x118 | QueueOverlayRect, states 4..8 | live per-value draw (no panel surface) | ARCH-DIV `[ARCH-DIV]` | accepted |
| Input idx0 resolution wrap | ordinal+=dir, wrap via table scan | :7895-7907 idx+=dir wrap mod count + apply_display_mode + save | MATCH (also applies live — orig defers to InitFrontendDisplayModeState) | minor: port applies mode immediately each tick |
| Input idx1 fog toggle &1 | (shadow+dir)&1 | :7908 `!s_display_fog_enabled` (toggle, ignores dir sign) | BUG (cosmetic) | port toggles regardless of dir; orig (shadow+dir)&1 — identical result for 2 states, harmless |
| Input idx2 speed &1 | (shadow+dir)&1 | :7912 `!s_display_speed_units` | BUG (cosmetic) | same as fog; harmless for 2-state |
| Input idx3 camera clamp[0,9] | +=dir clamp | :7915-7918 += delta clamp[0,9] | MATCH | — |
| Slide anim | frame 0x27 in / 0x10 out | :7877/7935 ms-timed | DIVERGENCE `[ANIM]` | low priority |

Class counts: MATCH 11, DIVERGENCE 4 (incl. label fmt + anim), BUG 2 (cosmetic), ARCH-DIV 2.

================================================================================
## Screen 17 — ScreenTwoPlayerOptions (0x00420C70)
Port: `Screen_TwoPlayerOptions` td5_frontend.c:7947 ; overlay
`frontend_render_two_player_options_overlay` td5_frontend.c:4153
================================================================================

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background + secondary cache | state0 | :7951 (MainMenu loaded inside frontend_load_tga? — see note) | MATCH `[ASSET]` | NOTE: :7953 loads SplitScreen.tga but case0 does NOT call frontend_load_tga("MainMenu") — background relies on the shared frontend BG. Verify the menu BG still draws (other screens explicitly reload it). |
| Header "2 PLAYER OPTIONS" | CreateMenuStringLabelSurface(6) | live render | ARCH-DIV `[LABEL]` | accepted fold |
| Row0 "SPLIT SCREEN" x=-0x100 w=0x100 +arrows(0,1) | 0x420d22 | :7955 -0x100/0x100 | MATCH | — |
| Row1 "CATCHUP" +arrows(1,1) | 0x420d33 SNK_CatchupTxt | :7957 "Catchup" | MATCH | — |
| Row2 "OK" x=-0x100 w=0x60, ESC idx=2 | 0x420d43 | :7959 -0x100/0x60 | MATCH | — |
| **Split-mode icon** src_y=mode<<5, 64×32, SplitScreen.tga@0x466094, states 3..8 | QueueOverlayRect @ (cx+0x4a, cy-0x8f) | :4163-4181 fe_draw_quad row=mode @ (394,97) | MATCH `[ASSET]` | — |
| **Split-mode NAME text** | dialog 0xe0×0x78: SNK_Split_Modes[mode] (state4) | :4160 on_off[mode] → "OFF"/"ON" | DIVERGENCE/BUG `[LABEL]` | orig shows the split-mode NAME (e.g. "HORIZONTAL"/"VERTICAL"), not ON/OFF. Port renders ON/OFF instead of the mode name. [UNCERTAIN: SNK_Split_Modes contents not byte-read] |
| **Catchup VALUE** | numeric 0..9 via sprintf template | :4161 on_off[catchup?1:0] → "OFF"/"ON" | BUG `[LABEL]` | orig shows the NUMERIC level 0..9; port collapses to ON/OFF, losing the level. Render the integer s_catchup_level. |
| Input idx0 split (mode+dir)&1 | 0x42106B | :7986 (delta+mode)&1 | MATCH | — |
| Input idx1 catchup clamp[0,9] | 0x4210B3 | :7996-7998 += delta clamp[0,9] | MATCH | — |
| OK confirm idx2 | 0x4210F7 | :8001 | MATCH | — |
| Slide anim incl. icon at +anim·0x20 | frame 0x27 in / 0x10 out | :7968/8012 ms-timed | DIVERGENCE `[ANIM]` | low priority |

Class counts: MATCH 9, BUG 2 (split-mode name, catchup numeric), DIVERGENCE 2, ARCH-DIV 1.

================================================================================
## Screen 18 — ScreenControllerBindingPage (0x0040FE00)
Port: `Screen_ControllerBinding` td5_frontend.c:8061 ; overlay
`frontend_render_controller_binding_overlay` td5_frontend.c:4550
================================================================================

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background + secondary cache | state0 | :8073 | MATCH | — |
| Header title | CreateMenuStringLabelSurface(6) | live render | ARCH-DIV `[LABEL]` | accepted fold |
| Per-device-class header text ("PRESS…","CONFIGURATION", rear-view/wheel variants) | PTR table @0x410c84 → 5 handlers; SNK_PressingTxt/ConfigurationTxt; hi-byte 0x600 wheel class | NOT rendered; port draws only a device icon + binding list | MISSING `[LABEL]` | port lacks the localized instruction/header strings and the wheel/pedal (0x600) variant. The PTR-dispatched header handlers are folded to ARCH-DIV (footer :10709). Add at least the "PRESS…/CONFIGURATION" instruction line. |
| Big binding panel 0x1c0×0xd8 + state strip 0x1c0×0x40 | CreateTrackedFrontendSurface, cleared | live text rows (no panel surface) | ARCH-DIV | accepted |
| **Device icon** | spec: original draws per-row binding panel; device icon is the Control-Options sheet, NOT this screen's primary art | :4554-4572 per-type TGA (Keyboard/Joypad/Joystick) centered y=120 | EXTRA/DIVERGENCE `[ASSET]` | port draws a single big per-type icon centered; spec screen 18 art is the binding LIST + ButtonLights, not a centered device icon. Likely a port liberty; verify against original screen render |
| **Live button-capture lights** (ButtonLights.tga@0x464068) | per-row QueueOverlayRect src_y=(pressed?0x10:0) 16×16, lit when row bit set; state 10 | NOT rendered | MISSING `[ASSET]` | the per-row lit/unlit capture indicators are absent. Port shows only text BTN labels. Add ButtonLights blits keyed on s_ctrl_js_curr bit per row. |
| Per-action binding row labels (SNK_ButtonTxt + digit; assigned-action) | DrawStringToSurface ×2 per row, count=g_controllerBindingButtonCount | :4594-4607 "BTN%d" + k_js_value_labels[bval-2] | DIVERGENCE `[LABEL]` | port "BTN1.." + placeholder value labels ("Axis+","Btn1"…); orig uses localized SNK_ButtonTxt/SNK_ControlText. k_js_value_labels marked [UNCERTAIN] (placeholders). |
| OK button x=-0x128 w=0x60 | 0x410165 | :8162 -0x128/0x60 | MATCH | — |
| Keyboard "PRESS KEY" prompt | SNK_PressKeyTxt (state 0x14) | :4625 "PRESS KEY FOR:" hardcoded | DIVERGENCE `[LABEL]` | port string is hardcoded English, not SNK_PressKeyTxt; close enough |
| **Keyboard action label** | SNK_ControlText[progress·0x10] (state 0x19), 10 actions | :4631 k_ctrl_action_labels[slot] | MATCH `[LABEL]` (labels fixed per MEMORY 2026-05-30) | confirm order: LEFT/RIGHT/ACCEL/BRAKE/HANDBRAKE/HORN/GEARUP/GEARDOWN/VIEW/REARVIEW ✓ |
| Keyboard progress "%d / 10" counter | not in original (orig shows one action, no counter) | :4636 "%d / 10" | EXTRA | port addition (harmless UX) |
| Device routing state0 (==3 keyboard, count by device val, hi 0x600 wheel) | dev byte==3→0x13; count=dev<4?2:dev<9?dev:8 | :8093-8158 td5_input_get_device_type; num_buttons hardcoded 8 | DIVERGENCE/BUG | port hardcodes num_buttons=8 (:8138) instead of reading device descriptor button count; wheel/pedal 0x600 class not handled |
| State 10 joystick edge cycle 2→10→2 | EdgeMask & Current; cache[row]++ wrap>10→2 | :8255-8265 | MATCH | — |
| State 10 count==2 axis swap | swap on 0x40000/0x80000 both frames | :8245-8251 | MATCH | — |
| State 10 joystick input source | DXInput::GetJS(devIdx-1) | :8218-8230 keyboard-arrow SURROGATE (no live GetJS) | BUG `[INPUT]` | port reads keyboard keys as a stand-in for joystick buttons (:8208 comment); real joystick binding does not work. Wire to td5_input joystick poll. |
| State 0x1a keyboard live-capture, dedup, Play(3) | scan 0..0xff, skip captured, first new → table[progress], Play(3) | :8352-8408 | MATCH | dedup + Play(3) faithful |
| Exit → SetFrontendScreen(14) | all exits | :8295/8428 TD5_SCREEN_CONTROL_OPTIONS | MATCH | — |
| Anim slide 0x1c | frame-count 0x1c in/out | :8180/8292 s_anim_tick≥0x1c | MATCH `[ANIM]` (frame-count, unlike screens 15-17) | — |

Class counts: MATCH 8, MISSING 2 (header text, ButtonLights), BUG 2 (num_buttons hardcode, JS-via-keyboard), DIVERGENCE 4, EXTRA 2, ARCH-DIV 2.

================================================================================
## Screen 19 — ScreenMusicTestExtras (0x00418460)  ⭐ DECOUPLED ALBUM ART
Port: `Screen_MusicTestExtras` td5_frontend.c:8443 ; overlay
`frontend_render_music_test_overlay` td5_frontend.c:4128 ;
helpers :4093 (track label) / :4102 (now-playing)
================================================================================

| element/behavior | ORIGINAL (spec+addr) | PORT (line) | CLASS | FIX |
|---|---|---|---|---|
| Background + secondary cache | state0 | :8459 frontend_load_tga MainMenu | MATCH | — |
| Header "MUSIC TEST" | CreateMenuStringLabelSurface(6) | live render | ARCH-DIV `[LABEL]` | accepted fold |
| **ALBUM / BAND COVER ART** | UpdateExtrasGalleryDisplay@0x40d830 (FLUSH, decoupled) → CrossFade16BitSurfaces; slide=(&g_extrasGallerySlideSurfaces)[LUT[track]]; LUT@0x465e4c=`01 03 04 04 02 00 00 01 03 04 04 04`; dest (0x76,0x8c)=(118,140); 5 band TGAs (Fear_Factory/Gravity_Kills/Junkie_XL/KMFDM/PitchShifter) via LoadExtrasBandGalleryImages; crossfade on band change | NOT loaded, NOT rendered (case0 :8449 LoadExtrasBandGalleryImages folded ARCH-DIV; no slide surfaces; no UpdateExtrasGalleryDisplay equivalent) | **MISSING `[DECOUPLED]` `[ASSET]`** | TOP FIX. Load the 5 band TGAs from Extras.zip into a port surface pool; on track SELECT (case6 idx0 confirm), pick slide via LUT@0x465e4c[track] and blit at (118,140); add the crossfade-on-band-change. Footer :10882 documents the fold. Note port k_music_test_band table is LUT-consistent (track0→Gravity Kills etc.). |
| Row0 "SELECT TRACK" x=-0x120 **w=0xA0 (160)** +arrows(0,1) | 0x418460 SNK_SelectTrackButTxt | :8462 "Select Track" -0x120/0xA0 | MATCH (width 0xA0 ✓) `[LABEL]` | label is correct width; verify "SELECT TRACK" fits 160px at the live font scale (the prompt's "wide-button/font" concern — see FONT note below) |
| Row1 "OK" x=-0x120 w=0x60, ESC idx=1 | 0x418460 | :8463 -0x120/0x60 | MATCH | — |
| **TRACK-# box "%d. %s"** | dialog 0x170×0x28: sprintf "%d. %s"@0x465f74 + MeasureOrCenter, baked state0 + re-bake on ◄►, @ (cx-0x32, cy-0x8f) | :4093-4100 label "%d. %s" idx+1 + band; :4135-4138 fe_draw_text @ (cx-0x32, cy-0x8f) | MATCH (live) | "%d. %s" + 0x32/0x8f offsets ✓ |
| **NOW-PLAYING box** (3 lines) | dialog 0x170×0x78: "NOW PLAYING" + band + title @ y=0/0x28/0x50, baked on SELECT confirm, @ (cx-0xc, cy-0x3f) | :4142-4150 fe_draw_text 3 lines @ (cx-0xc, cy-0x3f), y0/+0x28/+0x50 | MATCH (live) | offsets ✓; band/title tables match LUT |
| **Font of TRACK box / now-playing** | orig bakes into 0x170-wide surface, centered via MeasureOrCenterLocalizedString (small/localized atlas, color-keyed) | track label full-scale fe_draw_text (left-aligned at x, NOT centered in 0x170 box); "NOW PLAYING" at 0.8 scale | DIVERGENCE/BUG `[FONT]` | orig CENTERS each line within the 0x170 (368px) box; port draws left-aligned at the box origin and mixes scales (0.8 vs 1.0). This is the "font/width" issue. Center within 368px and use a consistent font scale. |
| Track cycle wrap 0..0xb | g_selectedCdTrackIndex+=dir wrap[0,0xb] | :8499-8506 wrap 0..11 | MATCH (music-wrap already fixed per prompt) | confirm ✓ |
| SELECT confirm → CDPlay(idx+2,1) + g_attractCdTrackCandidate=idx | 0x41864E | :8518 frontend_cd_play(idx) + update_now_playing | MATCH (cd_play); **MISSING** g_attractCdTrackCandidate write that drives album-art slide-in | BUG `[DECOUPLED]` | the band-art slide is keyed on g_attractCdTrackCandidate=selected track; without it (and without the art) the cover never changes. Tie into the album-art fix above. |
| OK → return scr15 + restore gallery | idx1→returnIndex=15, phase=0x40, state7; state8 Release+LoadExtrasGalleryImageSurfaces | :8523 state7; :8535 release/reload no-op | MATCH (return); ARCH-DIV (gallery restore folded) | accepted (no slideshow pool) |
| Slide anim 0x27 in / 0x20 out + crossfade phase | frame-count | :8481/8536 ms-timed; no crossfade phase | DIVERGENCE `[ANIM]` `[DECOUPLED]` | crossfade tied to album-art fix |

Class counts: MATCH 8, MISSING 1 (album art — TOP), BUG 2 (font/center, attract-candidate), DIVERGENCE 2, ARCH-DIV 3.

================================================================================
## CROSS-SCREEN SUMMARY
================================================================================

Per-screen CLASS counts:
- 15 Sound:    MATCH 12 | MISSING 1 | BUG 1 | DIVERGENCE 2 | ARCH-DIV 1
- 16 Display:  MATCH 11 | BUG 2 | DIVERGENCE 4 | ARCH-DIV 2
- 17 TwoPlayer:MATCH 9  | BUG 2 | DIVERGENCE 2 | ARCH-DIV 1
- 18 CtrlBind: MATCH 8  | MISSING 2 | BUG 2 | DIVERGENCE 4 | EXTRA 2 | ARCH-DIV 2
- 19 MusicTest:MATCH 8  | MISSING 1 | BUG 2 | DIVERGENCE 2 | ARCH-DIV 3

Cross-cutting tags:
- `[DECOUPLED]` — screen 19 album cover art (decoupled FLUSH draw) entirely absent;
  attract-candidate write missing. The live-render-vs-FLUSH model is a global ARCH-DIV
  but the album art is a genuine MISSING feature.
- `[FONT]` — screen 19 track/now-playing lines left-aligned + mixed scale vs orig
  centered-in-368px. Screen 18 value labels placeholder.
- `[LABEL]` — screen 17 split-mode shows ON/OFF not mode NAME; catchup shows ON/OFF
  not numeric. Screen 15 SFX-mode NAME text box missing. Screen 18 header strings +
  value labels not localized.
- `[ASSET]` — screen 19 5 band TGAs not loaded; screen 18 ButtonLights.tga not blitted.
- `[ANIM]` — screens 15/16/17/19 use ms-timed slides (DIVERGENCE) vs orig frame-count;
  screen 18 correctly uses frame-count.

TOP 3 FIXES (highest faithfulness impact):
1. **Screen 19 album/band cover art** [DECOUPLED][ASSET] — load 5 band TGAs, LUT@0x465e4c
   track→slide map, blit at (118,140) with crossfade-on-band-change; write
   g_attractCdTrackCandidate on SELECT. (MISSING — the prompt's headline gap.)
2. **Screen 17 value text** [LABEL] — render split-mode NAME (not ON/OFF) and the
   numeric catchup level 0..9 (not ON/OFF). Two genuine functional/label BUGs.
3. **Screen 18 ButtonLights + JS input** [ASSET][INPUT] — blit per-row capture lights
   and wire state-10 joystick reads to the real joystick poll instead of the keyboard
   surrogate (joystick rebind is currently non-functional). Plus screen 15 SFX-mode
   NAME text box (MISSING).

[UNCERTAIN] string contents not byte-read in the spec: SNK_SFX_Modes (15),
SNK_OnOffTxt/SNK_SpeedReadTxt/uiFormatTemplate (16), SNK_Split_Modes (17),
SNK_ButtonTxt/PressingTxt/ConfigurationTxt/PressKeyTxt + 0x600 device-desc (18),
full 12-entry band/title enumeration (19). k_js_value_labels (18) are placeholders.
