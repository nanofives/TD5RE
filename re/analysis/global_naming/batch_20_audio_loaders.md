---
batch: 20
area: audio_asset_loaders
tier: T4
target_todos: [todo_police_chase_no_audio_2026-05-19, reference_particle_render_audit_2026-05-17]
ghidra_session: bf0310baf0e145939edafbd373f9b7f9
analyzed_addresses: 0x00441a80, 0x00441c60, 0x00443280, 0x00441d50, 0x00441d90, 0x00414640, 0x00443240, 0x0042cbe0, 0x0042aa10, 0x0041ea90, 0x0043f420
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Audio asset loaders (DXSound buffer pool / per-vehicle banks)

## Summary

- Functions analyzed: 11 (5 primary T4.20 loaders + 6 callers/siblings reached via xref walk)
- Unnamed `DAT_*` globals encountered: 22 (after de-dup) across asset-string tables (0x00474a00..0x00474bbf), tuning constants (0x0045d5d0..0x0045d798), mesh-resource table (0x004c3d28..0x004c3d80), and frontend audio-options state (0x00465fe8..0x00465ff0)
- Already-named globals encountered (just noted): 13 (`gCarZipPathTable`, `gSlotCarTypeIndex`, `gSlotMeshResourcePtrTable`, `g_playerReflectionMeshResource`, `g_vehicleProjectionEffectMode`, `g_fixedPointToFloatScale`, `gVehicleTuningTable`, `gVehiclePhysicsTable`, `g_racerCount`, `gTrafficActorsEnabled`, `g_trackPoolIndex`, `g_selectedGameType`, `g_inputPlaybackActive`)
- Proposals — high confidence: 17
- Proposals — medium confidence: 5
- Proposals — comment-only (low confidence): 1

## Methodology

Entry points were the five audio-asset-loader functions named in the T4.20 brief:

| Address | Function | Role |
|---|---|---|
| `0x00441a80` | `LoadVehicleSoundBank` | Per-vehicle Drive/Rev/Horn buffer load (3 buffers per slot, called 6×) |
| `0x00441c60` | `LoadRaceAmbientSoundBuffers` | 19 ambient WAVs + traffic engine variants from SOUND.ZIP |
| `0x00414640` | `LoadFrontendSoundEffects` | 10-entry frontend SFX bank from Front End\Sounds\Sounds.zip |
| `0x00441d50` | `ReleaseRaceSoundChannels` | Race-end DXSound buffer purge (slots 0..43 removed, 44..87 stopped) |
| `0x00441d90` | `PlayVehicleSoundAtPosition` | 3D-positioned one-shot SFX play (the runtime listener-cursor reseat) |

From these I walked outward to the unique writer / caller functions for every global address referenced:

- `LoadRaceVehicleAssets @ 0x00443280` — sole caller of `LoadVehicleSoundBank`, also drives mesh allocation and the `IsLocalRaceParticipantSlot` decision that selects Rev vs Reverb
- `GetTrafficVehicleVariantType @ 0x00443240` — sole driver of the traffic-engine WAV variant index (lookup table at `0x00474ce8` keyed by `g_trackPoolIndex`)
- `IsLocalRaceParticipantSlot @ 0x0042cbe0` — used as the `param_3` argument to `LoadVehicleSoundBank`; selects Rev.wav (NULL) vs Reverb.wav (non-NULL)
- `InitializeRaceSession @ 0x0042aa10` — sole caller of `LoadRaceAmbientSoundBuffers`
- `InitializeFrontendResourcesAndState @ 0x00414740` — sole caller of `LoadFrontendSoundEffects`
- `ScreenSoundOptions @ 0x0041ea90` — the actual frontend audio-options sliders (DIFFERENT from `RunAudioOptionsOverlay @ 0x0043bf70` which T3.13 documented; this is the pre-race frontend variant)

Three structural insights drove the relevance gate:

1. **DXSound buffer pool is a 88-slot flat array** (44 base + 44 view-1 duplicates) — confirmed by the slot-id offsets in `LoadVehicleSoundBank` (slot 0..15 = vehicle banks, 16..17 = unused?, 0x12..0x24 = 18..36 ambient, 0x25.. = traffic variants), and `ReleaseRaceSoundChannels`'s `0..0x2b` Remove + `0x2c..0x57` Stop loops. The view-1 duplicate offset (`+0x2c` / 44) is hardcoded everywhere as `local_20 = 0x2c` in the mixer's view-1 pass. **The pool size is NOT a global** — it's a compile-time constant baked into immediate offsets across `UpdateVehicleAudioMix`, `ReleaseRaceSoundChannels`, and `PlayVehicleSoundAtPosition`.

2. **The Drive/Rev/Horn bank index for slot N is `N*3`** (slot 0 → buffers 0,1,2; slot 1 → 3,4,5; ... slot 5 → 15,16,17). This is hardcoded as `iVar3 = param_2 * 3` in `LoadVehicleSoundBank`. The view-1 duplicate is at `N*3 + 0x2c`. The mixer reads/writes these by computing `uVar10 * 3 + local_20` where `local_20` is 0 (view-0 pass) or 0x2c (view-1 pass).

3. **The "reverb mode" parameter is a STRING POINTER passed through three callers**: `LoadRaceVehicleAssets` calls `IsLocalRaceParticipantSlot(slot)` which returns `1` for the local-player slot (or 1/0 for split-screen first two slots). That return value (cast to `char *`) is passed as `param_3` to `LoadVehicleSoundBank`. The bank checks `param_3 == NULL` → Rev.wav (other actors); `param_3 != NULL` → Reverb.wav (the LOCAL player only). So `gVehicleSoundBankReverbModeByActor[slot] != NULL` is exactly equivalent to "this slot is a local participant" at race-init time, and `g_reverbVehicleActorIndex` (set by the same function) caches which slot got Reverb.wav. **In split-screen the "reverb player" is always slot 0** (the loop body for slot 1 also sets `DAT_004c3878 = iVar3 / 3` re-overwriting it, but in practice the order is sequential so the last local participant wins). This is the audio-side equivalent of `gPrimarySelectedSlot`.

## Proposals

### Audio asset string tables (constant arrays under 0x00474a00..0x00474bbf)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00474a00 | char*[19] | `g_ambientWavNameTable` | **high** | Read sequentially by `LoadRaceAmbientSoundBuffers` as `ppuVar5 = &PTR_s_Rain_wav_00474a00; do { ... } while (ppuVar5 < 0x474a4c)`. Loaded 19 buffers (76 bytes / 4 = 19 entries) into DXSound slots 18..36, with loop=true for first 4 (Rain, SkidBit, Siren3, Siren5). Port mirror: `s_ambient_wav_names[19]` at td5_sound.c:67. **CONFIRMED** by reading the 76-byte memory: 19 pointers, all valid string addresses under 0x00474a5b..0x00474b58. | td5_sound.c `s_ambient_wav_names` |
| 0x00474a4c | char*[3] | `g_trafficEngineVariantWavNameTable` | **high** | 3-entry pointer table (Engine0.wav, car.wav, diesel.wav) indexed by `GetTrafficVehicleVariantType` return value (0..2). Used by `LoadRaceAmbientSoundBuffers` when `g_racerCount > 6` (traffic actors active). Port mirror: `s_traffic_engine_wavs[3]` at td5_sound.c:90. | td5_sound.c `s_traffic_engine_wavs` |
| 0x00474a58 | float | `g_audio3dDistanceScale` | **high** | Bytes `0f 50 ee 3a` = `1.81818e-3f` ≈ 1/550. Read at 12 sites in mixer + `PlayVehicleSoundAtPosition` as `(listener.x - source.x) * _DAT_00474a58` — converts game-world fixed-point coordinate deltas into the float distance metric for attenuation/Doppler math. Ghidra has it labeled as 1-byte; **actually float32**. | td5_sound.c distance-attenuation constant (port has 0.5f equiv there) |
| 0x00474b60 | char[9] | `s_HornWavFilename` (already named) | n/a | "Horn.wav" — already labeled `s_Horn.wav_00474b60`; noted for completeness. | (asset constant) |
| 0x00474b6c | char[8] | `s_RevWavFilename` (already named) | n/a | "Rev.wav" — already labeled `s_Rev.wav_00474b6c`. | (asset constant) |
| 0x00474b74 | char[11] | `s_ReverbWavFilename` (already named) | n/a | "Reverb.wav" — already labeled `s_Reverb.wav_00474b74`. | (asset constant) |
| 0x00474b94 | char[10] | `s_DriveWavFilename` (already named) | n/a | "Drive.wav" — already labeled `s_Drive.wav_00474b94`. | (asset constant) |
| 0x00474bb0 | char[16] | `s_SoundZipPath` (already named) | n/a | "SOUND\\SOUND.ZIP" — already labeled `s_SOUND\SOUND.ZIP_00474bb0`. | (asset constant) |
| 0x004656f4 | char[28] | `s_FrontendSoundsZipPath` (already named) | n/a | "Front End\\Sounds\\Sounds.zip" — already labeled. | (asset constant) |

### Doppler/attenuation tuning constants (0x0045d5d0..0x0045d798)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d5d0 | float | `g_audioReplayDistanceMultiplier` | **high** | Bytes `0000003f` = 0.5f. Used at three loci: (a) `PlayVehicleSoundAtPosition` and mixer multiply final distance by this when `g_inputPlaybackActive != 0` (replay mode → 0.5× distance attenuation so audio is louder during replay); (b) the listener-velocity Doppler shift uses this on the `gTrackedVehicleAudioActorIndex` branch (siren in replay); (c) **ALSO reused by `LoadRaceVehicleAssets` UV-patch loop as `(uVar11 & 1) * _DAT_0045d5d0`** — non-audio overlap. Comment: "audio replay distance multiplier (0.5× attenuation when replay active)". | td5_sound.c replay-distance modifier |
| 0x0045d5f4 | float | `g_audioMinDistanceEpsilon` | high | Bytes `b9a5 5531` ≈ 3.10e-09f (tiny epsilon). Added to denominator in Doppler ratio: `fVar3 + _DAT_0045d5f4`. Prevents div-by-zero when listener and source are coincident. | td5_sound.c epsilon constant |
| 0x0045d604 | float | `g_audioDopplerSpeedOfSound` | high | Bytes `0000883f` = 1.0625f (?). Read as `* _DAT_0045d650 + _DAT_0045d604` in the Doppler ratio formula at four sites in the mixer. Functions as the "base" rate added to source-velocity term. Likely effective speed-of-sound in the game's fixed-point units. | td5_sound.c doppler base |
| 0x0045d624 | float | `g_audioDopplerZeroSentinel` | high | Bytes `0000803f` = 1.0f. Used as `if (fVar1 == _DAT_0045d624)` exact-equality check and lower clamp (`if (_DAT_0045d624 <= fVar1)`). Functions as "no Doppler shift" sentinel and minimum pitch ratio = 1.0 (no down-shift below normal). | td5_sound.c doppler floor |
| 0x0045d650 | float | `g_audioDopplerVelocityScale` | high | Bytes `00504348` = 200000.0f. Multiplier on listener-velocity dot product in Doppler ratio. Scales fixed-point velocity into a frequency-shift contribution. | td5_sound.c doppler velocity scale |
| 0x0045d6d8 | float | `g_audioDopplerMaxRatio` | high | Bytes `0000a041` = 20.0f. Upper clamp on Doppler frequency ratio: `if (_DAT_0045d6d8 < fVar1) { fVar2 = _DAT_0045d6d8; }`. Caps pitch shift at 20× to prevent extreme aliasing on high-speed approach. | td5_sound.c doppler max |
| 0x0045d790 | float | `g_audioSirenFrequencyScale` | med | Used only in the tracked-vehicle (siren) Doppler branch at 0x00441699: `_DAT_0045d798 + _DAT_0045d5f4 * _DAT_0045d790`. Likely a siren-specific pitch-shift scale (the siren plays at a different base freq than engines). | td5_sound.c siren frequency offset |
| 0x0045d794 | float | `g_audioEngineFrequencyScale` | med | Used in the engine-loop Doppler branch at 0x00440f3a: `(...) * (float)(int)local_14 * _DAT_0045d794`. Symmetric with `g_audioSirenFrequencyScale`. The engine branch scales the per-loop computed frequency (`local_14`); the siren branch uses a constant. | td5_sound.c engine frequency scale |
| 0x0045d798 | float | `g_audioDopplerOutputScale` | high | Final scalar on Doppler-clamped ratio before applying to source frequency: `(fVar2 - _DAT_0045d5f4) * _DAT_0045d798 + _DAT_0045d5f4`. Linearizes the clamped ratio back into a frequency-multiplier range. Used at 5+ mixer sites and in `PlayVehicleSoundAtPosition`. | td5_sound.c doppler output scale |

### Vehicle mesh resource cache (touched by `LoadRaceVehicleAssets`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3d28 | void*[6] | `gTrafficMeshResourcePtrTable` | high | Sister array of already-named `gSlotMeshResourcePtrTable`. `LoadRaceVehicleAssets` walks 6 traffic-mesh slots and writes `(&DAT_004c3d28)[iVar10] = iVar19` (per-mesh byte offsets into a single big heap alloc). Read by `0x0040bdb7` (traffic-actor mesh resolver, out of scope). | td5_track.c / td5_render.c traffic-mesh resolver |
| 0x004c3d48 | int[6] | `gSlotMeshResourceByteSize` | high | Per-slot mesh byte size cache. `LoadRaceVehicleAssets` writes `(&DAT_004c3d48)[iVar15] = uVar4` for slot iVar15 = 0..5 (or 0..1 in drag mode). Used to compute the second `ReadArchiveEntry` size argument when the same function re-reads `himodel.dat` for `g_playerReflectionMeshResource`. | (none — port consolidates) |
| 0x004c3d60 | int[6] | `gTrafficMeshResourceByteSize` | high | Twin of `gSlotMeshResourceByteSize` but for traffic slots. Written in the traffic-mesh allocation loop alongside `gTrafficMeshResourcePtrTable`. | (none) |
| 0x004c3d78 | void* | `g_chassisMeshArchiveEntry` | high | `LoadRaceVehicleAssets` writes `DAT_004c3d78 = FindArchiveEntryByName(piVar9, s_chassis_00474e14)` — caches the chassis-mesh entry header. Read at race-end cleanup `0x00443741`. The 4 floats at `+0x2c..+0x38` (read into `_DAT_004c3d7c..0x004c3d88`) are the chassis bounding-box for shadow + reflection. | td5_render.c chassis bbox |
| 0x004c3d7c | float | `g_chassisBoundsMinX` | med | First of 4 floats at `+0x2c..+0x38` of `g_chassisMeshArchiveEntry` (post-multiplied by `g_fixedPointToFloatScale`). Naming reflects probable role as a bounding-box edge but exact semantic unconfirmed (could be center vs edge). Used by shadow/reflection clip. | td5_render.c shadow extent |
| 0x004c3d80 | float | `g_chassisBoundsMaxX` | med | Second of the 4 chassis bounds floats. Speculative — could be max or width. | td5_render.c shadow extent |
| 0x004c3d84 | float | `g_chassisBoundsMinZ` | med | Third — same caveat. | td5_render.c shadow extent |
| 0x004c3d88 | float | `g_chassisBoundsMaxZ` | med | Fourth — same caveat. | td5_render.c shadow extent |

### Frontend audio-options state (0x00465fe8..0x00465ff0) — pre-race version of T3.13's RunAudioOptionsOverlay state

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00465fe8 | int | `g_sfxPlaybackMode` | **high** | 0..2 selector: 0 = stereo, 1 = mono?, 2 = 3D (only allowed if `DXSound::CanDo3D() != 0`). Written by `ScreenSoundOptions` cursor-row 0 navigation. Committed via `DXSound::SetPlayback(DAT_00465fe8)`. Read at 12 sites for UI rendering of the mode label and at race-init. Distinct from T3.13's audio-options-modal state. Persisted in Config.td5 (writers at `0x0040fd2e` = save, `0x0040f93f` = load). | td5_sound.c sfx playback mode |
| 0x00465fec | int | `g_sfxVolumePercent` | **high** | 0..100 percent. Written by `ScreenSoundOptions` cursor-row 1 navigation in steps of 10. Committed via `DXSound::SetVolume((DAT_00465fec * 0xfc00) / 100 & 0xfc00)` (converts 0..100 → 0..0xFC00 DSound volume units). Save/load at `0x0040fcd8`/`0x0040f983`. **Pre-race counterpart of T3.13's `g_sfxVolumeFraction @ 0x00474638`** — but THIS one is the percentage form persisted in Config.td5, and the modal version is a float fraction recalculated from it at runtime. | td5_sound.c sfx volume % |
| 0x00465ff0 | int | `g_musicVolumePercent` | **high** | Same as `g_sfxVolumePercent` but for CD music: `DXSound::CDSetVolume((DAT_00465ff0 * 0xfc00) / 100 & 0xfc00)`. Save/load at `0x0040fccc`/`0x0040f94a`. **Pre-race counterpart of T3.13's `g_cdAudioVolumeFraction @ 0x00466ea8`** — same percent/fraction pairing. | td5_sound.c music volume % |

## Key discoveries

1. **DXSound buffer pool layout is hardcoded as immediate offsets, not driven by globals.** Per-vehicle Drive/Rev/Horn buffers occupy slots `N*3 + (0..2)` for slot N in 0..5 (so buffer IDs 0..15 used, IDs 16,17 unused). Ambient/weather/hit-effect buffers occupy slots 18..36 (0x12..0x24). Traffic-engine variant loops occupy slots 37..42 (0x25..0x2a) when `g_racerCount > 6`. The view-1 duplicate set is +0x2c offset (slots 44..87). `ReleaseRaceSoundChannels` codifies this: it `DXSound::Stop(0x2c..0x57)` then `DXSound::Remove(0..0x2b)`. **The 44 / 88 split is a magic compile-time constant — there is no global named `g_audioPoolSize`.** Port-side `TD5_SOUND_TOTAL_SLOTS = 88` and `TD5_SOUND_DUP_OFFSET = 44` correctly mirror this.

2. **`LoadVehicleSoundBank(param_1=carZipPath, param_2=slot, param_3=isLocalParticipant)` semantically takes a BOOL as its third parameter but Ghidra surfaces it as `char *`.** That's because `IsLocalRaceParticipantSlot` returns `uint` and `LoadRaceVehicleAssets` casts it through `pcVar7 = (char *)IsLocalRaceParticipantSlot(uVar11)`. The bank checks `if (param_3 == NULL)` to pick Rev.wav vs Reverb.wav. **The interesting consequence**: in single-player only slot 0 gets Reverb; in split-screen slots 0 and 1 both get Reverb. The Rev branch (other actors) is what mutes the cross-actor engine reverb wash that would otherwise muddy the player's own engine sound — so the player gets the rich reverb-tail engine while opponents get the dry Rev.wav loop. **The `g_reverbVehicleActorIndex` (DAT_004c3878) is set to slot/3 = "the last slot that loaded the reverb bank"** — practically the last local participant.

3. **Race-time CD audio is started in `InitializeRaceSession`, not in any sound loader.** Confirms T3.13 finding that there's no race-time CD orchestrator. The CD-track that plays during a race is whatever `g_selectedCdTrackIndex` was set to at the end of `ScreenMusicTestExtras` or `ScreenSoundOptions`. `LoadRaceAmbientSoundBuffers` does NOT touch CD audio.

4. **Traffic engine variant selection is keyed by `g_trackPoolIndex` and a per-track 6-entry traffic vehicle list.** `GetTrafficVehicleVariantType(traffic_idx)` returns:
   - 0 (Engine0.wav) — track index ≥ 25 OR traffic disabled OR slot==4 (the "Viper" / sports car in traffic)
   - 1 (car.wav) — vehicle ID in traffic table != 14
   - 2 (diesel.wav) — vehicle ID in traffic table == 14 (truck/diesel type)
   The lookup `DAT_00474ce8 + (param_1 + DAT_00474d74[g_trackPoolIndex] * 6) * 4` reads a per-track traffic vehicle ID table starting at `0x00474ce8` indexed by `(track_traffic_table_index * 6 + slot_within_track)`. **Out-of-scope for this batch but flagged: `g_perTrackTrafficVehicleIdTable @ 0x00474ce8` and `g_perTrackTrafficTableIndex @ 0x00474d74` are T3 track-data candidates.**

5. **`PlayVehicleSoundAtPosition` does its OWN listener-cursor reseat each call** — not just per-mixer-pass. Each one-shot invocation walks viewports 0..1 (or just 0 in single-view), writes `DAT_004c387c` and `DAT_004c3828` to listener velocity/position pointers, computes attenuation+pan, then calls `DXSound::Play(slot_offset + param_1, ...)`. The `iVar6 = 0x2c` after the first viewport's play means the same buffer is played from BOTH viewports at the +0x2c duplicate slot. **The implication for the port**: every one-shot SFX must be loaded into BOTH the base slot and the +44 duplicate slot during init (which T3.13's `s_slot_to_buffer[i + TD5_SOUND_DUP_OFFSET] = buf_id` already handles).

6. **The `_DAT_0045d5d0` constant (0.5f) is "audio replay distance multiplier" — NOT just a UV-patch scalar.** `LoadRaceVehicleAssets` reuses this address for two unrelated purposes:
   - **Audio role**: `if (g_inputPlaybackActive != 0) fVar3 *= _DAT_0045d5d0;` — replay-mode attenuates distance by 0.5× so sounds are louder during recorded playback (since the camera tends to be further than during live play).
   - **UV-patch role**: per-actor `(uVar11 & 1) * _DAT_0045d5d0` adds 0.5f to UVs for odd-indexed slots — a tex-atlas offset selector.
   The same physical float gets read by two unrelated subsystems with completely different semantics. **Naming the address `g_audioReplayDistanceMultiplier` is preferred since the audio role is documented in 7+ mixer sites vs only 2 UV-patch sites; the mesh code should add an alias label.**

7. **The frontend audio-options screen (`ScreenSoundOptions @ 0x0041ea90`) is the persistence layer; the in-race overlay (`RunAudioOptionsOverlay @ 0x0043bf70` from T3.13) is a runtime modal.** Both touch volume but they have different state:
   - **Pre-race frontend** (`ScreenSoundOptions`): `g_sfxPlaybackMode` (0..2 modes), `g_sfxVolumePercent` (0..100), `g_musicVolumePercent` (0..100). Persisted in Config.td5.
   - **In-race modal** (T3.13's `RunAudioOptionsOverlay`): `g_cdAudioVolumeFraction` (float), `g_sfxVolumeFraction` (float), `g_engineSpeakerVolumeFraction` (float). NOT persisted directly — recomputed from the percentage form at race start.
   The port should mirror the percentage-form globals as the canonical source-of-truth and derive the float-form on-demand. **Confirmed by save-routine xrefs at `0x0040fccc..0x0040fd2e` writing only the percent forms to Config.td5.**

8. **`LoadVehicleSoundBank` issues 9 DXSound::LoadBuffer calls per car for 6 cars = 54 buffer loads in single-player race init, plus 19 ambient + up-to-6 traffic = 79 LoadBuffer total.** Each LoadBuffer is a synchronous staging-allocate→read→submit→free sequence; total race-init audio cost is ~80 ZIP entry reads. The `param_2 == 0` early-init clear (zeros 12+12+12+6 ints) happens ONLY on the first slot — subsequent slots only initialize their own slice. **This is the fragile behaviour T3.13 noted**: a partial reload (network late-join) leaves listener velocity, weather flags, and siren state stale.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00474ce8 | Per-track traffic-vehicle-ID lookup table (6-int rows, indexed by `g_trackPoolIndex`) | T3 traffic spawn batch |
| 0x00474d74 | Per-track traffic-table-row-index (1 dword per track, multiplied by 6 to index 0x00474ce8) | T3 traffic spawn batch |
| 0x00474df0 | Format string "model%d.prr" for traffic mesh entry names | T3 traffic mesh batch (asset string) |
| 0x00474dd8 | "traffic.zip" archive path constant | T3 traffic mesh batch (asset string) |
| 0x00474dfc | "carparam.dat" archive entry name | T3 vehicle physics batch (asset string) |
| 0x00474e0c | "TRAF%d" (model entry name format) | T3 traffic mesh batch |
| 0x00474e14 | "chassis" archive entry name | T3 vehicle mesh batch (asset string) |
| 0x00474e1c | "himodel.dat" archive entry name | T3 vehicle mesh batch (asset string) |
| 0x00465fec save/load sites at 0x004271cc/0x00427217 | These are sleep/idle setters in BENCHMARK mode that temporarily override `g_sfxVolumePercent` — a benchmark-mode volume floor | T3 benchmark batch |
| 0x004ae280 (`gVehiclePhysicsTable`) | Already named — confirmed by `LoadRaceVehicleAssets`'s copy of 32 dwords starting at carparam.dat+0x8c | (named) |
| 0x004ae580 (`gVehicleTuningTable`) | Already named — copy of 0x23 dwords (35) at carparam.dat+0 | (named) |
| 0x004ae8c8 | Traffic-vehicle tuning table base (0x23 dwords × 6 slots = stride 0x8c) — copies `gVehicleTuningTable` into per-traffic entries | T3 traffic-tuning batch |
| 0x0046cf6c-region | Per-track checkpoint table (referenced by `LoadTrackRuntimeData` in batch_10) | T3 track-data batch (already touched) |
| 0x004c376c | `_DAT_004c376c` — the "tracked-vehicle-extra audio-loop" companion field per T3.13's layout map; written to 0 by both `LoadVehicleSoundBank` and the mixer's viewport-layout transition | T3.13 follow-up / already mapped |
| 0x00474a58 + 4..N | Possibly more tuning floats packed adjacent to `g_audio3dDistanceScale` — Ghidra has not auto-classified bytes after 0x00474a58 | T4 audio-tuning-constants follow-up |
| 0x00455de0 / 0x00456110 | `WriteAudioToDirectSound` / `InitStreamDirectSound` — FMV/stream audio path (NOT the M2DX DXSound mixer; used by EA TGQ codec replacement) | T3 FMV batch |

## TODO impact

**todo_police_chase_no_audio_2026-05-19:** This batch confirms T3.13's verdict — the audio-asset side is COMPLETE. `LoadRaceAmbientSoundBuffers` correctly loads `Siren3.wav` (slot 18+2 = 20 = 0x14) and `Siren5.wav` (slot 18+3 = 21 = 0x15) as looping buffers (`iVar6 < 4` → loop=true). The port-side `s_ambient_wav_names` mirror at td5_sound.c:67 includes both. The view-1 duplicates at slot 0x14+0x2c = 0x40 and 0x15+0x2c = 0x41 are populated by the same load (the port's `slot_to_buffer[i + TD5_SOUND_DUP_OFFSET] = buf_id` aliasing). The mixer's siren-stop calls `DXSound::Stop(0x15)`, `Stop(0x16)`, `Stop(0x41)`, `Stop(0x42)` — covering both viewports for both siren tracks. **Naming `g_ambientWavNameTable` and confirming the slot 20/21 mapping makes the port-vs-original alignment auditable at a glance** for any future investigation if the siren mute behaviour drifts. **No new fix surfaces.** Apply batch_02's `g_wantedModeEnabled` writer to unlock the chain.

**Rain ambient not playing (T3.13 flagged):** Slot 0x12 (= 18) is Rain.wav (loop=true per the `iVar6 < 4` gate). Mixer's rain dispatch `DXSound::Play(local_20 + 0x12, ...)` is gated on `(&g_weatherActiveCountView0)[local_28] > 0 && g_weatherType == WEATHER_RAIN`. **The buffer load is byte-faithful and the slot number is correct (0x12).** If rain isn't playing in the port, the divergence must be one of: (a) `g_weatherActiveCountView0` never being incremented (track weather init), (b) `g_weatherType` not being set to `WEATHER_RAIN`, or (c) the port-side `td5_plat_audio_play` not honoring the `loop=1` parameter for slot 18. **Naming `g_ambientWavNameTable` makes the slot 18 = Rain.wav binding inspectable but doesn't itself fix the issue.** Recommend Frida trace `g_weatherActiveCountView0` and `g_weatherType` at race-init.

**Engine pitch wrong in replay (T3.13 flagged):** The `g_audioReplayDistanceMultiplier` (`_DAT_0045d5d0 = 0.5f`) is the ONLY value that differs between live and replay paths in the Doppler chain. Replay path multiplies the distance metric by 0.5× — making sources sound closer (louder + less-attenuated-pitch). The `g_audioEngineFrequencyScale` (`_DAT_0045d794`) is applied to the per-loop computed frequency in the engine branch (line 0x00440f3a). **If the port mishandles the `g_inputPlaybackActive` check, the engine pitch will be wrong by exactly that 0.5× factor through the Doppler distance term.** Recommend port-side check: ensure the replay-active flag gates the distance multiplier consistently across both pre-mixer and per-actor passes.

**reference_particle_render_audit_2026-05-17 (item 4: tire-track intensity floored at 0x30):** `UpdateFrontWheelSoundEffects` references `(&DAT_004c3725)[(uint)*(byte *)(...)*7]` — the tire-track emitter array is stride-7-byte structs with intensity at +0x05. This batch confirms the audio-asset loaders do NOT touch the tire-track emitter ring; the intensity-floor divergence is purely render-side. No naming change here, but **the 7-byte stride confirms the emitter pool is `DAT_004c375c` + N*0x7 + field, NOT N*4** — useful for the particle-render precise-port follow-up.

## Ghidra session notes

- Session `bf0310baf0e145939edafbd373f9b7f9` opened `TD5_pool0` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; release scheduled via `bash scripts/ghidra_pool.sh cleanup` after this file commits.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
