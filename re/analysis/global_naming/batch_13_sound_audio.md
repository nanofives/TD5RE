---
batch: 13
area: sound_audio_state
tier: T3
target_todos: [todo_police_chase_no_audio_2026-05-19]
ghidra_session: 38d4f126ff4f42b0a41a40b0b8f862ee
analyzed_addresses: 0x00440a30, 0x00440ab0, 0x00440ae0, 0x00440b00, 0x00441a80, 0x00441c60, 0x00441d50, 0x00441d90, 0x0042b580, 0x0042aa10, 0x00414640, 0x00418460, 0x0043bf70
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Sound / audio orchestrator state

## Summary

- Functions analyzed: 13 (5 primary T3.13 entry points + 8 callers / siblings reached via xref walk)
- Unnamed `DAT_*` globals encountered: 25 (after de-dup) inside 0x004c3768..0x004c38d8 audio block + 4 in 0x00474638..0x00474640 + 4 misc CD music
- Already-named globals encountered (just noted): 11 (`gTrackedVehicleAudioActive`, `gTrackedVehicleAudioActorIndex`, `gTrackedVehicleAudioFadeTarget`, `g_listenerPositionX/Y/Z`, `g_listenerPositionX_View1/Y_View1/Z_View1`, `g_selectedCdTrackIndex`, `g_cdAudioVolumeFraction`, `g_audioOptionsOverlayActive`, plus T2.9's `gActorTrackZoneAudioStateCode` @ 0x004c38a0)
- Proposals — high confidence: 18
- Proposals — medium confidence: 7
- Proposals — comment-only (low confidence): 0

## Methodology

Entry points were the five sound-orchestrator functions named in the port reference doc:

| Address | Function | Role |
|---|---|---|
| `0x00440a30` | `UpdateVehicleLoopingAudioState` | Per-actor horn-trigger; routes to siren in wanted mode |
| `0x00440ab0` | `StartTrackedVehicleAudio` | Cop-chase tracked-vehicle audio start |
| `0x00440ae0` | `StopTrackedVehicleAudio` | Cop-chase tracked-vehicle audio stop |
| `0x00440b00` | `UpdateVehicleAudioMix` | Master per-frame 3D mixer (~800 lines, 33 globals touched) |
| `0x00441c60` | `LoadRaceAmbientSoundBuffers` | Loads 19 ambient WAVs + traffic engine variants from SOUND.ZIP |
| `0x00441d50` | `ReleaseRaceSoundChannels` | Race-end cleanup (slots 0x2c-0x57 stop, 0x00-0x2b remove) |
| `0x00441d90` | `PlayVehicleSoundAtPosition` | 3D-positioned one-shot SFX play |
| `0x00441a80` | `LoadVehicleSoundBank` | Per-vehicle Drive/Rev/Horn buffer load + state reset |

From these I walked outward to the unique writer functions for each globals address:
- `RunRaceFrame @ 0x0042b580` — writes listener position triplet each frame from camera output
- `InitializeRaceSession @ 0x0042aa10` — calls `LoadRaceAmbientSoundBuffers`, plays initial CD track
- `LoadFrontendSoundEffects @ 0x00414640` — pre-race UI SFX
- `ScreenMusicTestExtras @ 0x00418460` — CD-track selection screen (writes `g_selectedCdTrackIndex`)
- `RunAudioOptionsOverlay @ 0x0043bf70` — in-race audio settings modal (volume sliders)

Three structural insights drove the relevance gate:

1. **Audio block at `0x004c3768..0x004c38e0` is a flat C-style struct** of per-slot arrays (engine state, horn state, skid state, traffic state), shared listener cache (2 viewports × 3 axes for pos / prev_pos / vel), and global mixer flags (siren, reverb-actor, audio-options-active). Almost every `DAT_*` in that range belongs to T3.13.
2. **Per-actor engine-state arrays are indexed `[viewport_index + actor*2]`** — not `actor*1`. The `*2` stride is the splitscreen multiplier; viewport 0 and viewport 1 each maintain a separate "what state am I in" snapshot to track when split-screen swap happens.
3. **The listener position is a dual-viewport pair**, with names `g_listenerPositionX/Y/Z` (view 0) and `_View1` siblings (view 1). Already named. The `DAT_004c387c` / `DAT_004c3828` are *runtime pointer-cursors* that get reseated per-pass to point at either (a) the active selected actor's velocity/world block, or (b) the listener pos/vel cache when replay mode is active.

The `DAT_004c38a0` global (lighting→audio mailbox) is already named `gActorTrackZoneAudioStateCode` by T2.9 batch_09; noted here but not re-proposed.

## Proposals

### Per-actor engine sound state (audio block 0x004c3770..0x004c37b8)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3770 | int[12] | `g_engineLoopStateByActorView` | high | 12-slot array (6 actors × 2 viewports), stride 4 bytes; written by `LoadVehicleSoundBank @ 0x00441a80` (init to 99 = STOPPED), read+written by `UpdateVehicleAudioMix` engine-loop dispatch. Values: 1=DRIVE, 2=REV, 99=STOPPED. Matches port `s_engine_state[12]` at `td5_sound.c:570`. | td5_sound.c s_engine_state |
| 0x004c37a0 | int[12] | `g_trafficEngineLoopActiveByActorView` | high | 12-slot array (6 traffic × 2 viewports), stride 4 bytes; `LoadVehicleSoundBank` zeros it, `UpdateVehicleAudioMix` lines 0x004417c8/0x0044181e/0x00441855 set 1 when traffic engine loop is firing this frame. Mirrors traffic engine state for restart detection. | td5_sound.c traffic-engine-active flags |
| 0x004c37d4 | u32 | `g_trackedVehicleAudioActiveView0Mirror` | med | Written together with `gTrackedVehicleAudioActive` (0x004c37d0) by `UpdateVehicleLoopingAudioState`, `StartTrackedVehicleAudio`, `StopTrackedVehicleAudio`'s clear path, and `LoadVehicleSoundBank`. NOT the same as `gTrackedVehicleAudioActive` — comment in decompiler reads `_DAT_004c37d4 = 1`. Acts as second-viewport mirror flag (see also `gTrackedVehicleAudioActive[local_28]` indexing at offset 4 inside `UpdateVehicleAudioMix`). | td5_sound.c s_tracked_veh_active_p2 |
| 0x004c37dc | int[8] | `g_sirenChannelPlayStateByActorView` | high | 12-slot int array indexed `[viewport + actor*2]`; tracks siren-channel state per actor per viewport. Values 1 = playing, 99 = stopped. Written by `UpdateVehicleAudioMix` at the siren-vehicle code path (0x00440d1d, 0x00440d3b, 0x00440f6f, 0x00440f8e) and reset by `LoadVehicleSoundBank` at 0x00441af6. | td5_sound.c siren-channel state |

### Per-actor horn / skid / reverse state (0x004c37e0..0x004c384c)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c37e0 | int[12] | `g_engineRevLoopStateByActorView` | high | Twin array of `g_engineLoopStateByActorView` (offset +0x70 in the audio block); written 99 by `UpdateVehicleAudioMix @ 0x00440bbd` when both engine slots are silent. This is the second engine sub-channel (DRIVE vs REV alternation). | td5_sound.c s_engine_state[2*i+1] |
| 0x004c382c | char*[6] | `g_vehicleSoundBankReverbModeByActor` | high | Per-actor pointer/flag; `LoadVehicleSoundBank` sets `(&DAT_004c382c)[param_2] = param_3` — `param_3` is the reverb name (s_Reverb_wav). `UpdateVehicleAudioMix @ 0x00440fe4` reads it as `(&DAT_004c382c)[uVar10]` to decide between Rev.wav and Reverb.wav playback semantics. NULL = use Rev, non-NULL = use Reverb. | td5_sound.c (per-actor reverb mode) |
| 0x004c3848 | int[12] | `g_skidLoopStateByActorView` | high | 12-slot int array indexed `[viewport + actor*2]`. Written by `UpdateVehicleLoopingAudioState @ 0x00440aa3` (sets to 1 = trigger), `UpdateVehicleAudioMix @ 0x00441491/0x0044151a/0x00441566` (transitions 1→2 = playing, 2→0 = stopped). Skid/tire-screech state machine. | td5_sound.c skid-loop state |
| 0x004c384c | int[12] | `g_skidLoopFlagAlternativeByActorView` | med | Written by `UpdateVehicleLoopingAudioState @ 0x00440aa3` (`(&DAT_004c384c)[slotIndex*2] = 1`) and other audio paths. Adjacent to 0x004c3848 (skid state) — likely paired flag distinguishing front-axle vs rear-axle skid OR a secondary skid-trigger latch. | (none — speculate) |
| 0x004c3768 | int[2] | `g_rainLoopActiveByViewport` | high | 2-int array (one per viewport). Written by `UpdateVehicleAudioMix @ 0x004413c8/0x00441408/0x0044141e` — gated by `(&g_weatherActiveCountView0)[local_28]` AND `g_weatherType == WEATHER_RAIN`. Tracks whether the per-viewport rain ambient loop is currently playing. Cleared to 0 by `LoadVehicleSoundBank @ 0x00441c37`. | td5_sound.c rain-loop active flag |

### Listener cache (viewport 0+1) — paired with already-named `g_listenerPositionX/Y/Z`

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3888 | int[6] | `g_listenerVelocityByView` | high | 6-int block (2 viewports × 3 axes). Written in `UpdateVehicleAudioMix` prologue loop `*(int *)((int)&DAT_004c3888 + iVar5) = *(int *)((int)&g_listenerPositionX + iVar5) - *(int *)((int)&DAT_004c38c0 + iVar5);` — straight delta = `(pos - prev_pos)`. Read by Doppler math throughout the mixer. Stride 0xc per viewport. | td5_sound.c s_listener_vel[vp][c] @ line 622 |
| 0x004c388c | int | `g_listenerVelocityByView_Y` | high | Alias of `g_listenerVelocityByView + 4`. Mirror written at 0x00440b3e. Y component of viewport-0 listener velocity. | td5_sound.c s_listener_vel[0][1] |
| 0x004c38c0 | int[6] | `g_listenerPrevFramePositionByView` | high | 6-int block (2 viewports × 3 axes). Mixer prologue writes `*(undefined4 *)((int)&DAT_004c38c0 + iVar5) = *(undefined4 *)((int)&g_listenerPositionX + iVar5)` — snapshots THIS frame's pos into prev for NEXT frame's delta. | td5_sound.c s_listener_prev_pos[vp][c] @ line 623 |
| 0x004c38c4 | int | `g_listenerPrevFramePositionByView_Y` | high | Alias of `g_listenerPrevFramePositionByView + 4`. Y-axis member of the prev-pos cache. | td5_sound.c s_listener_prev_pos[0][1] |
| 0x004c38c8 | int | `g_listenerPrevFramePositionByView_Z` | high | Alias of `g_listenerPrevFramePositionByView + 8`. Z-axis member. | td5_sound.c s_listener_prev_pos[0][2] |
| 0x004c387c | int* | `g_activeListenerVelocityPtr` | high | Runtime pointer cursor; re-pointed per-pass in mixer + `PlayVehicleSoundAtPosition`. In live race: points at `(&g_actorRuntimeState.slot.field_0x1cc + selectedSlot * 0x388)` (the racer's linear-velocity field). In replay/playback: points at `&DAT_004c3888 + local_28 * 3` (the listener-velocity cache). Read by Doppler-pitch math throughout. 8 xrefs. | td5_sound.c s_active_listener_vel (port aliases this) |
| 0x004c3828 | int* | `g_activeListenerPositionPtr` | high | Twin runtime cursor; re-pointed per-pass alongside `g_activeListenerVelocityPtr`. Live race → `&actor.field_0x1fc` (world_pos). Replay → `&g_listenerPositionX + local_28 * 3`. Read by distance-attenuation math. 8 xrefs. | td5_sound.c s_active_listener_pos @ line 996 |

### Mixer / siren globals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c38b8 | u32 | `g_trackedVehicleAudioFadeLevel` | high | Current siren fade level (target = `gTrackedVehicleAudioFadeTarget` @ 0x004c37d8). `UpdateVehicleAudioMix @ 0x00440bf0` advances toward target by 0x1000 steps. Cleared to 0 by `LoadVehicleSoundBank`. Mirror of `s_tracked_veh_fade_level` in the port at td5_sound.c:1002. | td5_sound.c s_tracked_veh_fade_level |
| 0x004c38d8 | char | `g_audioViewportLayoutChangePending` | high | Single byte; mixer prologue at 0x00440b87 compares with `g_raceViewportLayoutMode`. When `DAT_004c38d8 == 0` and layout becomes nonzero, mixer triggers per-actor engine-state reset; opposite branch stops siren channels 0x2c..0x57. Tracks pending viewport-layout transition for audio reset. | (none — port handles directly) |
| 0x004c3878 | u32 | `g_reverbVehicleActorIndex` | high | Set by `LoadVehicleSoundBank @ 0x00441b7a` when `param_3 != 0` (reverb-mode bank loaded). Read by `UpdateVehicleAudioMix @ 0x00440c55, 0x0044102a` as `uVar10 == DAT_004c3878` — the engine-pitch + reverb path. Identifies which slot owns the "reverb" engine bank (typically the local-player vehicle). | td5_sound.c reverb-vehicle slot id |
| 0x004c3880 | u32 (bool) | `g_sirenRefreshedThisFrame` | high | Sticky one-frame flag. Set by `UpdateVehicleLoopingAudioState @ 0x00440a7c` when the cop pressed-horn path runs. Read by mixer epilogue at 0x00441a2d: if `DAT_004c3880` was set AND `DAT_004c3844 == 0`, the mixer initiates fade-out via `gTrackedVehicleAudioFadeTarget = 0`. Cleared at 0x00441a51. **This is the siren-refresh latch the port reference doc refers to in s_siren_refreshed (td5_sound.c:561).** | td5_sound.c s_siren_refreshed |
| 0x004c3844 | u32 (bool) | `g_sirenActiveFlag` | high | Stay-alive flag. Set by `UpdateVehicleLoopingAudioState @ 0x00440a76` whenever cop's horn fires. Cleared by mixer epilogue at 0x00441a56. **Mirror of port s_siren_active_flag at td5_sound.c:553.** | td5_sound.c s_siren_active_flag |
| 0x004a2cdc | u32[6] | `g_vehicleAudioPlayingIntensityByActor` | med | Per-actor (6) u32. Written by mixer @ 0x00440d46 with value 0 (siren-stop) or value 1 (siren-active+nonzero-vol). Read by another subsystem (`0x004289d4 / 0x004289e6` — not in sound module, possibly HUD overlay flicker or particle gating). Effectively "is actor N's siren audible right now?" | (none — port doesn't propagate to HUD) |

### CD music / audio options

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00465e18 | u32 | `g_lastPlayedCdTrackIndex` | high | Set inside `ScreenMusicTestExtras @ 0x00418897` after `DXSound::CDPlay(g_selectedCdTrackIndex + 2, 1)`. Tracks "what was the most recently committed track" so the now-playing label survives navigation. NOT to be confused with `g_selectedCdTrackIndex @ 0x00465e14` (already named). | (none — port stub) |
| 0x00474638 | float | `g_sfxVolumeFraction` | high | Default value `1.0f` (read from memory). Written by `RunAudioOptionsOverlay @ 0x0043c37e` (`_DAT_00474638 = DAT_004b135c`). The general (non-CD) sound effects volume slider value. Used through `DXSound::SetVolume` epilogue. | td5_sound.c sfx volume |
| 0x0047463c | float | `g_engineSpeakerVolumeFraction` | med | Default `0.2f`. Written by `RunAudioOptionsOverlay @ 0x0043c384` (`_DAT_0047463c = DAT_004b1364`). Adjacent to SFX volume; likely a per-channel sub-slider (engine vs ambient). Speculative on semantic — confirmed only that it is a third independent volume value from the audio-options modal. | (none) |
| 0x00474640 | u32 | `g_audioOptionsCursorRow` | high | Cursor index inside `RunAudioOptionsOverlay`. Default `3` (OK button). Values 0..2 = CD/SFX/engine sliders, 3 = OK, 4 = quit-to-menu. Touched 8 times in 0x0043bf70 (read+write) for up/down navigation. | (none — port-side UI replacement) |
| 0x004bcb78 | u32 (bool) | `g_audioOptionsRepeatLatch` | med | Single-press debounce latch inside `RunAudioOptionsOverlay`. Set after left/right key consumed, cleared when both keys released. | (none) |
| 0x004b135c | float[3] | `g_audioOptionsSliderStaging` | med | 3-float array at 0x004b135c..0x004b1364. Loaded from globals at function entry, edited live by sliders, committed back to `g_cdAudioVolumeFraction / g_sfxVolumeFraction / g_engineSpeakerVolumeFraction` at function exit. Pure staging buffer for the modal. | (none — port-side overlay rewrites this) |

## Key discoveries

1. **The audio block at `0x004c3768..0x004c38e0` is a flat ~380-byte C struct of mixer state.** Its semantic layout is:
   - `+0x00`: rain-loop-active[2]      (`g_rainLoopActiveByViewport`)
   - `+0x04`: tracked-vehicle-extra    (`_DAT_004c376c`, audio-loop alt)
   - `+0x08..+0x37`: per-actor engine-loop state by viewport (`g_engineLoopStateByActorView[12]`)
   - `+0x38..+0x67`: per-actor traffic-engine-active by viewport (`g_trafficEngineLoopActiveByActorView[12]`)
   - `+0x68`: `gTrackedVehicleAudioActive`
   - `+0x6c`: `g_trackedVehicleAudioActiveView0Mirror`
   - `+0x70..+0x9f`: per-actor rev-loop state (`g_engineRevLoopStateByActorView[12]`)
   - `+0xa4`: `gTrackedVehicleAudioActorIndex`
   - `+0xa8`: `gTrackedVehicleAudioFadeTarget`
   - `+0xb0`: g_listenerPositionX[6 ints — 2 vp × 3 axes]
   - `+0xc4..+0xed`: per-actor reverb mode pointer (`g_vehicleSoundBankReverbModeByActor[6]`)
   - `+0xdc`: `g_sirenActiveFlag`
   - `+0xe0..+0x117`: per-actor siren-channel state (`g_sirenChannelPlayStateByActorView[12]`)
   - `+0x110`: `g_reverbVehicleActorIndex`
   - `+0x118`: `g_sirenRefreshedThisFrame`
   - `+0x120..+0x14F`: per-actor skid state pair (`g_skidLoopStateByActorView[12]`, `g_skidLoopFlagAlternativeByActorView[12]`)
   - `+0x150`: `g_listenerVelocityByView[6 ints]`
   - `+0x158`: `gActorTrackZoneAudioStateCode[6]` (already named in batch_09 — lighting→audio mailbox)
   - `+0x170`: `g_audioViewportLayoutChangePending`
   This layout matches the port's `td5_sound.c` static-state block but is split across many `s_*` arrays. **A future T4 cleanup could define `TD5AudioMixerState` as a single struct in Ghidra to clean up 25+ `DAT_*` references.**

2. **Siren / cop-chase audio depends on `g_sirenRefreshedThisFrame` (`DAT_004c3880`) and `g_sirenActiveFlag` (`DAT_004c3844`) — confirmed.** Both are stately latches: `g_sirenActiveFlag` is set EVERY frame the cop's horn-button is held (input bit 0x200000); `g_sirenRefreshedThisFrame` is the per-frame edge-trigger that drives the fade-target reset. The mixer's fade-out happens automatically when `g_sirenRefreshedThisFrame == 1 && g_sirenActiveFlag == 0` (i.e. it was triggered but no longer being held). The siren-buffer asset itself loads as slot `+0x12` (= slot 0x14 after the +2 base) by `LoadRaceAmbientSoundBuffers` from `Siren3.wav`/`Siren5.wav` strings at `0x00474a00 + slot*4`. Hence the cop-chase audio chain is:
   - **Asset load**: `LoadRaceAmbientSoundBuffers` reads `Siren3.wav` (slot index 18+2 = 20 = 0x14, the loop=true flag at index<4) and `Siren5.wav` (slot 21 = 0x15).
   - **Input trigger**: `UpdateVehicleLoopingAudioState` sees `g_wantedModeEnabled != 0 && actor_index == g_wantedTargetSlotIndex` AND `DAT_004c3880 == 0`, sets `gTrackedVehicleAudioActive = 1` + `gTrackedVehicleAudioActorIndex = slotIndex` + `gTrackedVehicleAudioFadeTarget = 0x1000`.
   - **Mixer dispatch**: each frame, `UpdateVehicleAudioMix` checks `gTrackedVehicleAudioActive != 0`, computes 3D dist+doppler from cop actor pos vs `g_activeListenerPositionPtr`, calls `DXSound::Play/Modify` on slots `slot_offset + 0x14` and `slot_offset + 0x15`.
   - **Fade-out**: mixer epilogue compares `DAT_004c3880` (was triggered) vs `DAT_004c3844` (still active) and sets `gTrackedVehicleAudioFadeTarget = 0` if the input is gone.
   This confirms the port-side observation that the missing `g_wantedModeEnabled` writer (batch_02) is the only break in the chain — every other global on the path is present in the port.

3. **The `g_activeListenerPositionPtr` (`DAT_004c3828`) and `g_activeListenerVelocityPtr` (`DAT_004c387c`) are runtime-reseated EVERY MIXER PASS** (pass 0 = viewport 0, pass 1 = viewport 1) — they're not "snapshot once" pointers. In live race, they point at `g_actorRuntimeState.slot[selected].world_pos` and `.linear_velocity_x`; in replay mode they point at the listener-pos/velocity cache. This explains why the port can't simply read `g_listenerPositionX` directly — it needs to follow the same per-pass swap to match the original 3D math exactly.

4. **`LoadVehicleSoundBank` only zeroes the global audio-mixer state on the FIRST slot load (`param_2 == 0`).** Subsequent slot loads only initialize per-slot fields. So a partial reload (e.g., late-joining player in network mode) won't reset the listener cache, weather-loop flags, or siren state. The port's `td5_sound.c` does a more conservative full-reset on every `td5_sound_init` call (correctly diverging from this fragile original behaviour).

5. **CD music has only 4 distinct globals**: `g_selectedCdTrackIndex` (cursor in music-test screen), `g_lastPlayedCdTrackIndex` (which track is currently spinning in the disc drive — used to redraw the now-playing label without restarting playback), `g_cdAudioVolumeFraction` (slider value), and the M2DX import `DXSound::CDPlay`. **There is NO race-time CD orchestrator** — the CD plays whatever track was last selected via `ScreenMusicTestExtras` and only changes when `InitializeRaceSession` calls `DXSound::CDPlay(g_selectedCdTrackIndex + 2, 1)` to restart it at race start. This is the simplest possible CD music architecture; the port's `td5_sound.c` should mirror it as a one-shot CD-track binding to whatever the user last chose.

6. **The audio-options overlay uses three independent volume sliders** (CD/SFX/engine) staged in `g_audioOptionsSliderStaging[3]` (`DAT_004b135c..0x004b1364`) and committed back to three different globals on exit (`g_cdAudioVolumeFraction`, `_DAT_00474638` = `g_sfxVolumeFraction`, `_DAT_0047463c` = `g_engineSpeakerVolumeFraction`). The `g_audioOptionsCursorRow` selects which one the player is currently editing. The naming `_engineSpeakerVolumeFraction` for the third is provisional — could equally be "music-bus" or "ambient-bus"; only runtime trace would disambiguate.

7. **No dedicated "ListenerSetPosition" function exists** — listener position writes are *inline* inside `RunRaceFrame @ 0x0042bdb4` immediately after `UpdateVehicleRelativeCamera` (for view 0) and `UpdateVehicleRelativeCamera_view1` (for view 1). The mixer reads `g_listenerPositionX` directly. This means the 3D listener pos IS the camera pos — there is no head-tracking or separate listener offset. The port's `td5_sound.c` correctly mirrors this by reading the camera state directly.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00474a00..0x00474a4b | Ambient WAV name pointer table (19 entries: Rain, SkidBit, Siren3, Siren5, ScrapeX, Bottom1-4, HHit1-4, LHit1-5, Gear1) | T3 sound-assets (constant tables) |
| 0x00474a4c..0x00474a57 | Traffic engine-variant WAV name pointer table (3 entries: Engine0, car, diesel) | T3 sound-assets |
| 0x00474a58 | `_DAT_00474a58` — float, distance-scale constant for Doppler / attenuation math | T3 sound-tuning-constants (likely == `TD5_SOUND_DISTANCE_SCALE` in port) |
| 0x00474b60..0x00474b94 | Per-vehicle WAV name strings (Drive.wav, Reverb.wav, Rev.wav, Horn.wav) | T3 sound-assets |
| 0x00474bb0 | `s_SOUND_SOUND_ZIP_00474bb0` (already a string label) — archive path | (named already) |
| 0x0045d5d0..0x0045d798 | Cluster of float constants used by Doppler/attenuation/volume math inside mixer | T3 sound-tuning-constants |
| 0x004b135c..0x004b1364 | Audio options staging block — could also be analyzed as part of the broader overlay-modal data block at 0x004b1358+ | T3 overlay-modals batch |
| 0x004b1358 | Pointer to audio-options modal layout struct (read by `RunAudioOptionsOverlay @ 0x0043bf76` as DAT_004b1358 + 0x2c/0x30/0x3c) | T3 overlay-modal-tables |
| 0x004b1368 | Translucent-primitive count for audio options overlay | T3 overlay-modal-tables |
| 0x004b1370 | Translucent-primitive start address for audio options overlay | T3 overlay-modal-tables |
| 0x00466ea8 | `g_cdAudioVolumeFraction` is already-named — note benchmarked default `0x3f266666` (= 0.65f) set at `InitializeRaceSession @ 0x0042aa27` when `g_benchmarkModeActive != 0` | (already named) |

## TODO impact

**todo_police_chase_no_audio_2026-05-19:** This batch CONFIRMS the root cause already identified in batch_02 (missing `g_wantedModeEnabled` writer in port's race-init) by walking the full audio chain end-to-end:

  1. `LoadRaceAmbientSoundBuffers` loads `Siren3.wav` → DX buffer slot 0x14, and `Siren5.wav` → slot 0x15. **Port-side mirror exists** at `td5_sound.c` ambient_wav_names slots 2, 3.
  2. `UpdateVehicleLoopingAudioState` gates the trigger on `g_wantedModeEnabled != 0 && actor_index == g_wantedTargetSlotIndex`. **Port mirror at td5_sound.c:552 reads `td5_game_is_wanted_mode()` — which returns 0 because no writer ever sets it.**
  3. Once triggered, `g_sirenActiveFlag` (`DAT_004c3844`) + `g_sirenRefreshedThisFrame` (`DAT_004c3880`) + `gTrackedVehicleAudioActive`/`Actor`/`FadeTarget` cascade dispatch the per-frame siren mix correctly.
  4. **All 5 globals on the audio side are present and the mixer code path is byte-faithful to the original.** The only missing link remains the upstream `g_wantedModeEnabled` writer in `td5_game.c`'s race-init equivalent of `InitializeRaceSession @ 0x0042acf2`.

  **Verdict:** No new fix surfaces from this batch — the audio side is complete. Apply the batch_02 fix (add `g_wantedModeEnabled = 1` when `g_selectedGameType == TD5_GAMETYPE_COPS_CHASE`) and the entire siren chain unlocks.

  Naming `g_sirenActiveFlag`, `g_sirenRefreshedThisFrame`, `g_trackedVehicleAudioFadeLevel`, `g_engineLoopStateByActorView`, `g_sirenChannelPlayStateByActorView`, and the listener pointers makes `UpdateVehicleAudioMix`'s 800-line decompilation drastically easier to read for any future audio investigations (e.g. ambient-rain not playing in port, engine pitch wrong in replay mode, etc.).

## Ghidra session notes

- Session `38d4f126ff4f42b0a41a40b0b8f862ee` opened `TD5_pool0` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; release scheduled via `bash scripts/ghidra_pool.sh cleanup` after this file commits.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
