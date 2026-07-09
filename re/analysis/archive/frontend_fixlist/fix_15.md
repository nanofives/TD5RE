# Frontend Fix List — Screens 15-19 (3-layer faithfulness audit)

RESEARCH ONLY. Implementation-ready, 3-layer (CREATION / RENDERING / BEHAVIOR-INPUT)
verification of the 5 sound/display/2-player/binding/music-test screens against the PORT.

Inputs verified against:
- ORIGINAL spec: `re/analysis/frontend_screens/screens_15.md`
- ORIGINAL binary (Ghidra, read-only): ScreenSoundOptions 0x41EA90, ScreenDisplayOptions 0x420400,
  ScreenTwoPlayerOptions 0x420C70, ScreenControllerBindingPage 0x40FE00, ScreenMusicTestExtras 0x418460
  — all 5 decompiled fresh this pass.
- SNK_* strings: `re/analysis/frontend_snk_strings.md` (byte-faithful from Language.dll).
- PORT: `td5mod/src/td5re/td5_frontend.c` (worktree fix-1780170781-137846-13319).

PORT dispatch anchors (verified):
- overlay dispatch `switch(s_current_screen)` @ td5_frontend.c:5588 — all 5 screens present.
- arrow dispatch `switch(s_current_screen)` @ td5_frontend.c:5784 — all 5 present.
- active_button fallback = `(s_button_index >= 0) ? s_button_index : s_selected_button`.

Byte-read constants resolved this pass (closing prior [UNCERTAIN]):
- `SNK_SFX_Modes` = ["MONAURAL","STEREO","3D SOUND"]  (0x100071E4).
- `SNK_Split_Modes` = ["LEFT/RIGHT","UP/DOWN"]  (0x100071DC).
- `SNK_OnOffTxt` = ["OFF","ON"]; `SNK_SpeedReadTxt` = ["MPH","KPH"].
- catchup/camera-damping value template `g_uiFormatStringScratchTemplate` @0x465498 = **"%d"** (bare integer).
- `SNK_SelectTrackButTxt` = **"TRACK"**; `SNK_ButtonTxt` = "BUTTON"; `SNK_PressingTxt` =
  "PRESS A BUTTON TO CHANGE THE"; `SNK_ConfigurationTxt` = "CONFIGURATION FOR THAT BUTTON";
  `SNK_PressKeyTxt` = "PRESS THE KEY TO USE FOR".
- music LUT @0x465e4c = `01 03 04 04 02 00 00 01 03 04 04 04` (matches port k_music_track_to_band).

================================================================================
### Screen 15 — ScreenSoundOptions  [interactive Y]
Flow: 0 init/load (5 buttons SFX Mode/SFX Vol/Music Vol/Music Test/OK; loads Controllers/VolumeBox/
VolumeFill TGAs; bakes SFX-mode-name into 0xe0×0xa0 dialog) → 3 slide-in → 4 bake mode-name → 6
interactive → 7/8 slide-out → SetFrontendScreen(return). ESC idx=4.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| 5 button rows + widths/labels | "SFX Mode/SFX Volume/Music Volume/Music Test/OK" -0x100..-0x60 w 0x100/0x60 @:7810-7814 ✓ | n/a | n/a | MATCH (labels match SNK_* uppercased) | — |
| SFX-mode icon (Controllers.tga row mode+4 of 7, 64×32 @394,97) | loaded :7807 ✓ | rendered :4056-4070 rows (mode+4)/7 ✓ | n/a | MATCH | — |
| Master/CD volume bars (BG 224×12 + FILL ∝vol, suppress 0) | n/a | :4075-4101 fe_draw_quad, fill_w=vol/100·222 ✓ | n/a | MATCH | — |
| **SFX-mode NAME text** ("MONAURAL"/"STEREO"/"3D SOUND") baked into dialog (state 4/6) @ (394,97) | orig bakes SNK_SFX_Modes[mode] | **NOT rendered** (:4041 comment "no extra text needed") | n/a | **MISSING (L2)** | td5_frontend.c:4070 (after icon block) add `fe_draw_text_centered` of `k_sfx_mode_names[mode]` ({"MONAURAL","STEREO","3D SOUND"}) centered in the 224-wide dialog at x=394+112, y≈97+icon (orig centers via MeasureOrCenter offset 0x4a inside 0xe0). The icon and name share the (394,97) dialog region — place name just below the 32px icon. |
| 3-mode vs 2-mode (CanDo3D) | orig state6 wraps 0/1 if !CanDo3D else 0/1/2 | n/a | port wraps 0/1/2 unconditional :7843-7845 | DIVERGENCE (backend always 3D-capable, accepted :7838) | — |
| SFX mode → DXSound::SetPlayback(mode) | orig calls SetPlayback | n/a | port mutates s_sound_option_sfx_mode only, NO playback-route call :7843 | BUG (minor, L3) | td5_frontend.c:7845 after clamp add `td5_sound_set_sfx_mode(s_sound_option_sfx_mode)` (verify such an API exists in td5_sound; the icon already changes, but the audio routing mode does not). |
| Volume input ±10 clamp[0,100] + persist | orig ±dir·10, SetVolume/CDSetVolume | n/a | :7849-7860 ±delta·10 clamp + td5_save/td5_sound ✓ | MATCH | — |
| Music Test (idx3) confirm → scr19; OK (idx4) → hub | n/a | n/a | :7864 raw s_button_index==3; :7867 ==4 ✓ (orig also uses raw g_frontendButtonIndex for confirm) | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? Y :5598-5599; arrow dispatch? Y :5795-5796 (idx0-2);
input uses active_button fallback? Y :7835 (delta path); confirm path uses raw s_button_index==3/4 (faithful — orig identical).

SCREEN VERDICT: faithful except 1 MISSING (SFX-mode NAME text) + 1 minor BUG (no SetPlayback call).
Ordered fixes: (1) render SFX-mode name text [L2 MISSING]; (2) call sfx-playback-mode setter [L3 minor].

================================================================================
### Screen 16 — ScreenDisplayOptions  [interactive Y]
Flow: 0 init (Resolution/Fogging/SpeedReadout/CameraDamping/OK; Fogging is a live cycler iff
DXD3D::CanFog()==1 else a DISABLED preview button) → 3 slide-in → 4 bake value strings → 6 interactive
→ 7/8 slide-out → hub. ESC idx=4.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| 5 button rows (-0x120 w0x120, OK w0x60) | :7905-7915 ✓ | n/a | n/a | MATCH | — |
| Resolution value (display-mode string) | n/a | :4032 s_display_mode_names[idx] else "UNAVAILABLE" | n/a | DIVERGENCE (port "%dx%d %dbpp" vs orig table string; accepted ARCH-DIV) | — |
| Fogging value ON/OFF (only baked if CanFog) | orig SNK_OnOffTxt[shadow] | :4033 on_off[fog&1] always | n/a | MATCH (CanFog==1 always on D3D11; accepted :7906-7911) | — |
| Speed-readout value MPH/KPH | orig SNK_SpeedReadTxt[units] | :4034 {"MPH","KPH"}[units&1] ✓ (now byte-confirmed) | n/a | MATCH | — |
| Camera-damping value (numeric 0..9 via "%d") | orig sprintf "%d" | :4031/4035 snprintf "%d" ✓ | n/a | MATCH | — |
| Fogging row preview/disabled when !CanFog | orig CreateFrontendDisplayModePreviewButton + NO value, NO arrows | port always live button :7912 | n/a | DIVERGENCE (accepted; backend always fogs) | — |
| idx0 resolution wrap + apply + save | orig ordinal+=dir table-walk wrap | n/a | :7943-7955 wrap mod count + apply + save ✓ | MATCH (port applies live; orig defers — cosmetic) | — |
| idx1 fog toggle, idx2 speed toggle, idx3 camera clamp | orig (shadow+dir)&1 / clamp | n/a | :7957 `!fog`, :7961 `!speed`, :7963-7966 clamp ✓ | MATCH (2-state toggle == (shadow+dir)&1; harmless) | — |
| OK idx4 → hub | n/a | n/a | :7968 raw s_button_index==4 ✓ | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? Y :5604-5605; arrow dispatch? Y :5792-5793 (idx0-3);
input uses active_button fallback? Y :7940 (port adds the fallback the ORIG lacks — orig uses raw
`switch(g_frontendButtonIndex)`; the fallback is a benign port improvement for keyboard arrows,
consistent with the music-test fix — NOT a divergence to undo).

SCREEN VERDICT: fully faithful within accepted backend divergences. NO action items (0 fixes).
All prior [UNCERTAIN] strings byte-confirmed MATCH this pass.

================================================================================
### Screen 17 — ScreenTwoPlayerOptions  [interactive Y]
Flow: 0 init (Split Screen/Catchup/OK; SplitScreen.tga icon) → 3 slide-in → 4 bake BOTH split-mode
name AND catchup "%d" into ONE 0xe0×0x78 dialog → 6 interactive → 7/8 slide-out → hub. ESC idx=2.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| 3 button rows ("Split Screen"/"Catchup"/"OK") | :8003-8007 ✓ | n/a | n/a | MATCH | — |
| Split-screen MODE icon (SplitScreen.tga row=mode<<5, 64×32 @394,97) | loaded :8001 ✓ | rendered :4207-4221 v_row by mode ✓ | n/a | MATCH | — |
| **Split-mode NAME value** ("LEFT/RIGHT" / "UP/DOWN") | orig bakes SNK_Split_Modes[mode] | **:4201 renders on_off[mode]→"OFF"/"ON"** | n/a | **BUG (L2 wrong string)** | td5_frontend.c:4195 add `const char *split_modes[]={"LEFT/RIGHT","UP/DOWN"};` and :4201 change to `split_modes[s_split_screen_mode & 1]`. |
| **Catchup VALUE** (numeric 0..9 via "%d") | orig sprintf "%d" of g_twoPlayerCatchupAssist | **:4202 renders on_off[catchup?1:0]→"OFF"/"ON"** | n/a | **BUG (L2 collapses level→ON/OFF)** | td5_frontend.c:4202 replace with `char buf[8]; snprintf(buf,sizeof buf,"%d",s_catchup_level); frontend_draw_value_centered(..., buf, ...)`. |
| idx0 split (mode+dir)&1; idx1 catchup +=dir clamp[0,9] | orig confirmed | n/a | :8034 (delta+mode)&1; :8044-8046 clamp ✓ | MATCH | — |
| OK idx2 → hub | n/a | n/a | :8049 raw s_button_index==2 ✓ | MATCH | — |

PORT-WIRING CHECK: render-overlay dispatch? Y :5607-5608; arrow dispatch? Y :5798-5799 (idx0-1);
input uses active_button fallback? Y :8031.

SCREEN VERDICT: 2 genuine L2 label/value BUGs (both shown as ON/OFF instead of the mode-NAME and the
numeric level). Ordered fixes: (1) catchup numeric value [most user-visible — level is invisible];
(2) split-mode name string.

================================================================================
### Screen 18 — ScreenControllerBindingPage  [interactive Y]  ⭐ partly ARCH-DIV
Flow: 0 init/device-route (reads g_player1/2DeviceDesc[deviceIdx]; ==3→keyboard path; else joystick;
button count = desc<4?2:desc<9?desc:8; hi-byte 0x600 = wheel/pedal header variant) → 9 slide-in → 10
joystick live-capture (GetJS, ButtonLights, per-row cycle 2→10) → 0xb out; OR 0x13 kb slide-in → 0x14
init → 0x19 action label → 0x1a live scancode capture → 0x1b out. ALL exits → SetFrontendScreen(14).

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| OK button (-0x128 w0x60) | :8210 ✓ | n/a | n/a | MATCH | — |
| **Header instruction text** ("PRESS A BUTTON TO CHANGE THE" + "CONFIGURATION FOR THAT BUTTON") | orig bakes SNK_PressingTxt + SNK_ConfigurationTxt into 0x1c0×0xd8 panel (state0) | **NOT rendered** | n/a | **MISSING (L2)** | td5_frontend.c:4591 (controller-binding overlay) draw two centered lines "PRESS A BUTTON TO CHANGE THE" / "CONFIGURATION FOR THAT BUTTON" above the binding list during the joystick path (s_inner_state==10). |
| **Per-row ButtonLights** (ButtonLights.tga 16×16, src_y=0x10 when row-bit set, x≈cx-200) | orig loads ButtonLights.tga; per-row QueueOverlayRect | **NOT rendered** (port draws only text) | n/a | **MISSING (L2/asset)** | load ButtonLights.tga in case0 (:8118 block); in the state-10 row loop :4635 add a 16×16 quad per row at (96,row_y) using v0=0.5 when `(s_ctrl_js_curr & (0x40000<<i))` else v0=0.0 (sheet is two 16px rows). |
| Per-row label "BUTTON%d" + assigned-action value | orig SNK_ButtonTxt+digit ("BUTTON1") + action string | :4640 "BTN%d"; :4646 k_js_value_labels[bval-2] (placeholders) | n/a | DIVERGENCE (L1/L2 label text) | td5_frontend.c:4640 change "BTN%d" → "BUTTON%d" (faithful SNK_ButtonTxt). Value-label content :464-474 [UNCERTAIN] — orig second-draw source is the live binding's localized action; mapping value→string not byte-confirmed; leave placeholder OR map 2/3→"AXIS+/AXIS-" and 4..10→SNK_ControlText-style. |
| Keyboard "PRESS KEY FOR" prompt | orig SNK_PressKeyTxt="PRESS THE KEY TO USE FOR" | :4666 "PRESS KEY FOR:" hardcoded | n/a | DIVERGENCE (L1 label; close) | td5_frontend.c:4666 optionally change to "PRESS THE KEY TO USE FOR" to match SNK_PressKeyTxt. Low priority. |
| Keyboard action label (10 actions LEFT..REAR VIEW) | orig SNK_ControlText[progress·0x10] | :4672 k_ctrl_action_labels[slot] ✓ (verified matches SNK_ControlText[0..9]) | n/a | MATCH | — |
| "%d / 10" progress counter | not in original | :4677 added | n/a | EXTRA (harmless port UX) | — (leave) |
| Device button count | orig desc<4?2:desc<9?desc:8 from g_player(1/2)DeviceDesc | n/a | :8186 hardcodes num_buttons=8 | DIVERGENCE/BUG (L3) | td5_frontend.c:8186 derive count from the real device descriptor button count instead of constant 8; clamp <4→2, >=9→8. Needs a td5_input API exposing the device button count. |
| State10 joystick INPUT source | orig DXInput::GetJS(deviceIdx-1) | n/a | :8260-8278 **keyboard-arrow SURROGATE** (no live GetJS) | BUG (L3 INPUT) — joystick rebind non-functional | td5_frontend.c:8260 replace keyboard surrogate with the real joystick poll (td5_input packed GetJS for the active device; the multi-device joystick support from branch fix-1780168877 provides the OUTPUT, not merged into THIS worktree). |
| State10 edge cycle 2→10→2 / count==2 axis swap | n/a | n/a | :8293-8313 ✓ | MATCH (logic faithful; only the source bits are surrogate) | — |
| State0x1a scancode capture, dedup, Play(3) | orig scan 0..0xff dedup, Play(3) | n/a | :8400-8463 ✓ | MATCH | — |
| Exit → screen 14 | n/a | n/a | :8343/8476/8481 ✓ | MATCH | — |

NOTE — ARCH-DIV (exclude from counts): the PTR-dispatched header handler table @0x410c84 and the
hi-byte 0x600 wheel/pedal header variant are folded to ARCH-DIV in the port (single-icon model).
The decoupled state-strip/binding-panel offscreen surfaces → live render is the project-wide ARCH-DIV.

PORT-WIRING CHECK: render-overlay dispatch? Y :5619-5620; arrow dispatch? N (this screen has no
◄►-cyclable display-mode rows — orig has no InitializeFrontendDisplayModeArrows here; correct to omit);
input uses active_button fallback? N/A (confirm uses raw s_button_index==0 for OK :8318 — faithful;
binding edits are driven by joystick-bit rising edges / scancode scan, not arrow nav).

SCREEN VERDICT: most-divergent screen. 2 MISSING (L2: header text, ButtonLights) + 1 INPUT BUG
(L3: joystick via keyboard surrogate — rebind non-functional) + label/count divergences. Ordered:
(1) wire real joystick poll [L3, makes the screen actually work]; (2) ButtonLights per-row [L2];
(3) header instruction text [L2]; (4) "BUTTON%d" label + device-derived button count.

================================================================================
### Screen 19 — ScreenMusicTestExtras  [interactive Y]  ⭐ DECOUPLED ALBUM ART (now implemented)
Flow: 0 gallery-fade-gated init (loads band gallery; bakes track-# box "%d. %s" + now-playing box
"NOW PLAYING:"+band+title) → 3 slide-in → 4 queue boxes → 6 interactive → 7/8 slide-out (restore
slideshow) → scr15. ESC idx=1.

| element/behavior | L1 creation | L2 render | L3 behavior | VERDICT | FIX (file:line + change) |
|---|---|---|---|---|---|
| Button row0 "TRACK" (-0x120 w0xA0) + row1 "OK" (w0x60) | :8515-8516 "TRACK"/"OK", w 0xA0/0x60 ✓ (SNK_SelectTrackButTxt="TRACK", prior "Select Track" fixed) | n/a | n/a | MATCH | — |
| **ALBUM / BAND COVER ART** (5 band TGAs; band=LUT[track]; blit @(118,140)) | loaded :8519-8528 (Fear Factory/Gravity Kills/Junkie XL/KMFDM/PitchShifter from Extras.zip) ✓ | rendered :4151-4170 via k_music_track_to_band[idx] @(118,140) ✓ | keyed on **live cursor** s_music_test_track_idx | **DIVERGENCE/BUG (L3)** — orig keys cover on `g_attractCdTrackCandidate` (set ONLY on SELECT confirm), so cover changes only when SELECT is pressed; port changes it immediately while ◄►-cycling | td5_frontend.c add `s_music_attract_track` (init=0; set = s_music_test_track_idx on SELECT confirm :8590); :4152 use `k_music_track_to_band[s_music_attract_track]` instead of `[s_music_test_track_idx]`. (Cover then follows the PLAYED band, not the previewed one — faithful to 0x418460 case6.) NOTE: album art itself is no longer MISSING (was the prior headline gap; now present). |
| Crossfade-on-band-change (phase 0x100→halve→-0x40) | orig CrossFade16BitSurfaces | port plain opaque draw :4158 (no crossfade) | n/a | DIVERGENCE (L2 cosmetic; decoupled-flush ARCH-DIV) | optional: add a short alpha fade when the band index changes. Low priority. |
| Track-# box "%d. %s" centered in 0x170 box @ (cx-0x32, cy-0x8f) | label built :4106-4113 ✓ | rendered centered :4174-4178 ✓ | re-baked on ◄► :8579 ✓ | MATCH | — |
| Now-playing box (3 lines NOW PLAYING/band/title) @ (cx-0xc, cy-0x3f) y0/+0x28/+0x50 | built :4115-4123 ✓ | rendered :4183-4191 ✓ | re-baked on SELECT :8591 ✓ | MATCH | — |
| Now-playing scale mix ("NOW PLAYING:" 0xFFCCCCCC vs full white) | orig all baked same atlas | :4188 same font scale (sx,sy) for all 3 lines | n/a | MATCH (prior "mixed-scale" concern resolved — all 3 use sx/sy now) | — |
| Track cycle ◄► wrap 0..0xb (NO clamp) | orig wrap | n/a | :8571-8578 wrap 0↔11 ✓ (music-wrap fix verified) | MATCH | — |
| SELECT (idx0) → CDPlay(idx+2,1) + set attract-candidate + re-bake now-playing | orig CDPlay + g_attractCdTrackCandidate=idx + re-bake | n/a | :8582-8593 frontend_cd_play + update_now_playing ✓ BUT does NOT set an attract-candidate (see album-art row) | BUG (L3) — tie into album-art fix | as above: add `s_music_attract_track = s_music_test_track_idx;` at :8590. |
| OK (idx1) → return scr15 (phase reset, restore gallery) | n/a | n/a | :8595 state7; :8607-8615 release/reload no-op (no slideshow pool) | MATCH (return) / ARCH-DIV (gallery restore folded) | — |

PORT-WIRING CHECK: render-overlay dispatch? Y :5601-5602; arrow dispatch? Y :5818-5823 (idx0 — was
the prior missing-from-dispatch creation≠render gap, NOW FIXED); input uses active_button fallback?
Y :8565 (the track-cycle bug — NOW FIXED; confirm/OK use raw s_button_index==0/1 which is faithful).

SCREEN VERDICT: the three prior headline gaps are CLOSED — album art now loads+renders, arrows wired
@:5823, track-cycle uses active_button @:8565, label is "TRACK". One remaining L3 divergence: cover
follows the LIVE cursor instead of the last-SELECTED track (orig g_attractCdTrackCandidate semantics).
Ordered fixes: (1) gate cover art on a SELECT-set attract-track var [L3]; (2) optional crossfade [L2 cosmetic].

================================================================================
## SUMMARY (ARCH-DIV excluded from fix counts)
================================================================================

| Screen | interactive | L1 fixes | L2 fixes | L3 fixes | top fix | ARCH-DIV present |
|---|---|---|---|---|---|---|
| 15 Sound      | Y | 0 | 1 (SFX-mode NAME text) | 1 (SetPlayback call) | render SFX-mode NAME text | header-label live-fold |
| 16 Display    | Y | 0 | 0 | 0 | none (faithful) | header fold; res-string fmt; live-value model |
| 17 TwoPlayer  | Y | 0 | 2 (split-name + catchup numeric) | 0 | catchup NUMERIC value (level invisible as ON/OFF) | header fold; live-value model |
| 18 CtrlBind   | Y | 1 (BUTTON%d label) | 2 (header text, ButtonLights) | 2 (JS-via-kbd surrogate, hardcoded btn count) | wire real joystick poll (rebind non-functional) | PTR header table + 0x600 wheel class; live-surface model |
| 19 MusicTest  | Y | 0 | 0 (cover now present; +1 optional crossfade) | 1 (cover keyed on live cursor not SELECTed track) | gate cover on SELECT-set attract var | header fold; gallery restore; decoupled-flush model |

Total non-ARCH-DIV fixes: L1=1, L2=5, L3=4  (=10 actionable items, +2 optional cosmetic L2).

Cross-screen TOP fix: **Screen 18 joystick poll** — the binding screen's interactive purpose
(rebinding a joystick) is currently non-functional (keyboard surrogate). Second: **Screen 17 catchup
numeric value** (the 0..9 level is completely hidden behind ON/OFF). Both are genuine functional bugs,
not cosmetic.

[UNCERTAIN] remaining: S18 per-row value-label content (orig second-draw localized action source not
byte-mapped to binding value 2..10); k_js_value_labels are placeholders. S18 0x600 wheel/pedal header
variant device-desc byte values not enumerated.
