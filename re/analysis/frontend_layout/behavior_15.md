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
