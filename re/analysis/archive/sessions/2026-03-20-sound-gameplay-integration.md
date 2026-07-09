# Sound-Gameplay Integration in TD5_d3d.exe

**Date:** 2026-03-20
**Binary:** TD5_d3d.exe (Ghidra port 8193), M2DX.dll (port 8192)
**Scope:** Complete analysis of how the DXSound subsystem integrates with race gameplay: slot assignments, trigger conditions, pitch/volume formulas, spatial audio, and music management.

---

## 1. Architecture Overview

TD5 uses a **two-tier sound architecture**:
- **DXSound** (M2DX.dll): 44 base slots + 44 duplicate slots (88 IDirectSoundBuffer objects), plus CD audio via wv2wav/MCI backend. See `re/sessions/2026-03-12-dxsound-subsystem.md` for full DXSound API.
- **TD5 sound manager** (TD5_d3d.exe): Manages per-vehicle sound banks, spatial attenuation, Doppler-like pitch shifting, and the per-frame audio mix.

**Key DXSound APIs used by TD5:**
- `DXSound::LoadBuffer(data, slot, loop, nDuplicates)` -- load WAV into slot
- `DXSound::Play(slot, vol, freq, pan)` -- start playback with parameters
- `DXSound::Modify(slot, vol, freq, pan)` -- update running sound
- `DXSound::ModifyOveride(slot, vol, freq, pan)` -- update ignoring mute gate
- `DXSound::Stop(slot)` -- stop playback
- `DXSound::Remove(slot)` -- release buffer
- `DXSound::Status(slot)` -- query playing state
- `DXSound::CDPlay(track, loop)` -- play CD audio track
- `DXSound::CDSetVolume(vol)` / `DXSound::SetVolume(vol)` -- master volumes
- `DXSound::MuteAll()` / `DXSound::UnMuteAll()` -- race pause muting

---

## 2. DXSound Slot Map

### 2.1 Per-Vehicle Sound Banks (Slots 0-17, duplicated at 44-61)

Each of the 6 race vehicles occupies **3 consecutive slots**:

| Slot Base | Content | Loop | Duplicates | Description |
|---|---|---|---|---|
| `vehicle*3 + 0` | Drive.wav | Yes (1) | 2 | Engine driving sound (high RPM) |
| `vehicle*3 + 1` | Rev.wav or Reverb.wav | Yes (1) | 2 | Engine idle. If `IsLocalRaceParticipantSlot` returns non-null, loads Reverb.wav instead of Rev.wav for the local player's reverb/proximity variant |
| `vehicle*3 + 2` | Horn.wav | No (0) | 2 | Vehicle horn |

**Slot assignments for 6 vehicles:**

| Vehicle | Drive | Rev/Reverb | Horn |
|---|---|---|---|
| 0 (player 1) | 0 | 1 | 2 |
| 1 (player 2 / AI) | 3 | 4 | 5 |
| 2 | 6 | 7 | 8 |
| 3 | 9 | 10 | 11 |
| 4 | 12 | 13 | 14 |
| 5 | 15 | 16 | 17 |

**Split-screen duplication:** Slots 44-61 mirror slots 0-17 for the second viewport (pan offset = +10000 for player 2's audio stream).

### 2.2 Ambient/Effect Sound Slots (Slots 18-30)

Loaded by `LoadRaceAmbientSoundBuffers` (0x441c60) from `SOUND\SOUND.ZIP`:

| Slot | WAV File | Loop | Purpose |
|---|---|---|---|
| 0x12 (18) | Rain.wav | Yes (1) | Rain ambient loop |
| 0x13 (19) | SkidBit.wav | Yes (1) | Tire screech/skid loop |
| 0x14 (20) | Siren3.wav | Yes (1) | Police siren variant 3 |
| 0x15 (21) | Siren5.wav | Yes (1) | Police siren variant 5 |
| 0x16 (22) | ScrapeX.wav | No (0) | Metal scrape (wall contact) |
| 0x17 (23) | Bottom3.wav | No (0) | Undercarriage bottoming out |
| 0x18 (24) | Bottom1.wav | No (0) | Undercarriage hit variant 1 |
| 0x19 (25) | Bottom4.wav | No (0) | Undercarriage hit variant 4 |
| 0x1A (26) | Bottom2.wav | No (0) | Undercarriage hit variant 2 |
| 0x1B (27) | HHit1.wav | No (0) | Heavy collision hit 1 |
| 0x1C (28) | HHit2.wav | No (0) | Heavy collision hit 2 |
| 0x1D (29) | HHit3.wav | No (0) | Heavy collision hit 3 |
| 0x1E (30) | HHit4.wav | No (0) | Heavy collision hit 4 |

**Continued (slots loaded after first 13):**

| Array Index | Slot | WAV File | Loop | Purpose |
|---|---|---|---|---|
| 13 | 0x1F (31) | LHit1.wav | No (0) | Light collision hit 1 |
| 14 | 0x20 (32) | LHit2.wav | No (0) | Light collision hit 2 |
| 15 | 0x21 (33) | LHit3.wav | No (0) | Light collision hit 3 |
| 16 | 0x22 (34) | LHit4.wav | No (0) | Light collision hit 4 |
| 17 | 0x23 (35) | LHit5.wav | No (0) | Light collision hit 5 |
| 18 | 0x24 (36) | Gear1.wav | No (0) | Gear shift clunk |

The pointer table at `0x474a00` holds 19 pointers, yielding **slots 0x12-0x24** (18-36).

**Note:** Looping flag = `(index < 4)`, so the first 4 entries (Rain, SkidBit, Siren3, Siren5) are looping; the rest are one-shot.

### 2.3 Traffic Vehicle Engine Slots (Slots 37+)

For traffic actors beyond the 6 race slots (indices 6+), `LoadRaceAmbientSoundBuffers` loads additional engine loops into slot `index + 0x25` (37+):

| Variant Type | WAV File | Assignment |
|---|---|---|
| 0 | Engine0.wav | Generic engine |
| 1 | car.wav | Car engine |
| 2 | diesel.wav | Diesel/truck engine |

`GetTrafficVehicleVariantType` (0x443f10) selects the variant based on NPC group table data. Returns 0 for highway/generic, 1 for standard car, or 2 if the traffic model ID is 0x0E (large vehicle/truck).

### 2.4 Frontend Sound Effects (Slots 1-10)

Loaded by `LoadFrontendSoundEffects` (0x414640) from `Front End\Sounds\Sounds.zip`:

| Slot | WAV File | Purpose |
|---|---|---|
| 1 | ping3.wav | Menu tick/low |
| 2 | ping2.wav | Menu tick/mid |
| 3 | Ping1.wav | Menu tick/high |
| 4 | Crash1.wav | Menu crash/select |
| 5 | Whoosh.wav | Menu transition slide |
| 6 | Whoosh.wav | Menu transition (duplicate) |
| 7 | Crash1.wav | Menu confirm (duplicate) |
| 8 | Whoosh.wav | Menu transition (duplicate) |
| 9 | Crash1.wav | Menu crash (duplicate) |
| 10 | Uh-Oh.wav | Error/warning sound |

### 2.5 Slot Lifecycle

**Race shutdown** (`ReleaseRaceSoundChannels`, 0x441d50):
1. Stops duplicate-range slots 0x2C-0x57 (44-87) via `DXSound::Stop`
2. Removes base-range slots 0x00-0x2B (0-43) via `DXSound::Remove`

---

## 3. Per-Frame Sound Update Pipeline

### 3.1 Call Order in RunRaceFrame (0x42b580)

At the **end of each frame**, after EndRaceScene:

```
1. For each player slot with horn button pressed (bit 0x200000):
     UpdateVehicleLoopingAudioState(actorIndex)
2. UpdateVehicleAudioMix()
```

### 3.2 UpdateVehicleAudioMix (0x440b00) -- The Master Sound Mixer

This is the **core sound function**, ~800 lines. Called once per frame after rendering. It processes:

**A. Listener velocity computation:**
- Computes listener velocity delta vectors from camera position changes (`DAT_004c3810/3814/3818` -> `DAT_004c3888/388c/3890`)
- Stores previous positions for next-frame delta

**B. Split-screen viewport transition:**
- Detects viewport mode changes; resets playback states when switching between 1P and 2P

**C. Tracked vehicle audio (siren/pursuit):**
- Fade target system: `gTrackedVehicleAudioFadeTarget` drives instant 0/0x1000 transitions
- When `gWantedModeEnabled` and player is near cop (`DAT_004bf51c`), activates siren audio on slots 0x15/0x16 (and 0x41/0x42 for split-screen)

**D. Per-vehicle engine sound update (the main loop):**
For each of 6 race vehicles, on each viewport pass:

#### Engine Pitch Formula

```c
// Raw speed from actor physics state (field +0x310 = smoothed engine speed)
raw_speed = actor.gap_0310[0];

if (speed < 1000 && reverb_vehicle) {
    // Diesel/reverb mode: fixed low-frequency idle
    volume_raw = 0x50;       // ~63% of max
    pitch = 0x5622;          // 22050 Hz (native sample rate)
} else {
    // Normal engine mode
    speed_scaled = (raw_speed / 4) + (is_tracked_vehicle + 2) * 0x400;
    // is_tracked_vehicle adds +0x400 base, viewer vehicle adds extra +0x400

    if (actor != reverb_actor && gear_state > 0) {
        speed_scaled += 0x400;  // +1024 for engaged gear
    }

    volume_raw = clamp(speed_scaled, 0, 0xFFF) >> 5;  // 0-127 range

    // Pitch = ((rand() % 100 + raw_speed) * 0x67) / 0x23 + 10000
    // Simplifies to: pitch = (rand_jitter + speed) * 2.826 + 10000
    // This gives roughly 10000-40000 Hz range
    pitch = ((rand() % 100 + raw_speed) * 103) / 35 + 10000;
}
```

**Random jitter:** Each frame adds `rand() % 100` to the raw speed before the pitch calculation, creating natural engine sound variation.

#### Spatial Audio (3D Positioning)

For **non-viewer vehicles**, the mixer computes:

```c
// Distance in 2D (XZ plane) from listener to vehicle
dx = (listener.x - vehicle.x) * SCALE_FACTOR;  // 0x474a58 = ~0.001817
dz = (listener.z - vehicle.z) * SCALE_FACTOR;
distance = sqrt(dx*dx + dz*dz);

// During replay: distance halved (camera closer to action)
if (replay_mode) distance *= 0.5;

// Volume attenuation: linear falloff over 127 units
vol_atten = (0x7F - (round(distance) >> 7)) * volume_raw / 0x7F;
vol = clamp(vol_atten, 0, 0x7F);

// Doppler-like pitch shift (relative velocity)
// Projects vehicle velocity and listener velocity onto line-of-sight
v_vehicle = dot(vehicle.velocity, normalize(dx,dz));
v_listener = dot(listener.velocity, normalize(dx,dz));

// Doppler ratio: clamped to [0.0, 2.0]
doppler_raw = (v_listener * 4.0 + 4096.0);
if (doppler_raw != 0.0)
    doppler_ratio = (v_vehicle * 4.0 + 4096.0) / doppler_raw;
else
    doppler_ratio = v_vehicle * 4.0 + 4096.0;
doppler_ratio = clamp(doppler_ratio, 0.0, 2.0);

// Final pitch adjustment: +-7.4% range around base pitch
pitch_final = round(((doppler_ratio - 1.0) * 0.074074 + 1.0) * pitch);
```

**Scale factor `0x474a58`** = `0x3AEE500F` = approximately 0.001817, converting world-space fixed-point coordinates to audio distance units.

#### Viewer Vehicle (Local Player)

The local player's engine sound uses:
- **Direct volume** from speed (no distance attenuation)
- **Steering-based pan** for single-player: `pan = steering_command * -0x51EB851F >> 38` (subtle left/right shift based on wheel turn, only at speed > 999)
- Engine sound plays on the Drive slot (odd/even swap based on gear state)
- Rev/Reverb slot provides idle undertone

#### Engine Sound State Machine

Per vehicle, per viewport, a state byte tracks which engine sample is active:
- State `99`: Stopped (initial). On first update, starts the appropriate engine loop.
- State `1`: Drive.wav active (high-speed engine). Uses `DXSound::Play` then `DXSound::Modify`.
- State `2`: Rev/Reverb.wav active (low-speed/idle). Used when `raw_speed < 1000` and vehicle has reverb flag.

When the state changes (e.g., speed crosses 1000 threshold), the old slot is stopped and the new one started.

### 3.3 Horn Volume Switch Table

When the horn is active for the local player (`DAT_004c3de0 != 0`), the horn slot plays at fixed frequency 0x5622 (22050 Hz). Volume is set from `DAT_004c3de0` (clamped to 0x7F).

The in-race audio overlay `RunAudioOptionsOverlay` uses `DXSound::ModifyOveride` to preview the horn at volume 100 when the "Music Test" (actually sound test) row is selected.

### 3.4 Horn Sound Gear-Volume Table

In `UpdateVehicleAudioMix`, the horn volume level maps from `DAT_004c38a0` (gear state):

| Gear State | Horn Volume |
|---|---|
| 0 (default) | 10 |
| 1 | 90 |
| 2 | 80 |
| 3 | 60 |
| 4 | 40 |
| 5 | 20 |

---

## 4. Collision Sound Triggers

### 4.1 Vehicle-to-Vehicle Collisions

Triggered in `ApplyVehicleCollisionImpulse` (0x4079c0). The function computes `iVar10` = absolute impulse magnitude. Sound is triggered only if at least one participant is a race vehicle (index < 6):

| Condition | Sound Slot Base | Volume | Pitch | Random Variants |
|---|---|---|---|---|
| `iVar10 >= 0x3201` (12801) | 0x1F (LHit1-5) | 0x1000 (full) | 0x0D02 (3330 Hz) | 5 (random LHit1-5) |
| `iVar10 >= 0xC801` (51201) | 0x1B (HHit1-4) | 0x1000 (full) | 0x2198 (8600 Hz) | 4 (random HHit1-4) |

The `PlayVehicleSoundAtPosition` function handles **random variant selection**: `if (numVariants > 1) slot += rand() % numVariants`.

### 4.2 Wall/Barrier Scraping

Triggered in `ApplyTrackSurfaceForceToActor` (0x406980). When lateral force `iVar10` exceeds 0x3200:

| Condition | Sound Slot | Volume | Pitch | Variants |
|---|---|---|---|---|
| Lateral force 0x3201-0x19000 | 0x16 (ScrapeX) | force/8, clamped 0x400-0x800 | 0x5622 (22050 Hz) | 1 |
| Lateral force > 0x19000 | 0x1B (HHit1-4) | force/8, clamped 0x400-0x800 | 0x2198 (8600 Hz) | 4 |

Additionally triggers force feedback via `PlayAssignedControllerEffect` and `DecayUltimateVariantTimer` for the scoring system.

### 4.3 PlayVehicleSoundAtPosition (0x441d90) -- Spatial Sound Dispatcher

Used by both collision and scrape sounds. Parameters: `(baseSlot, volume, pitch, position*, numVariants)`.

**Key behavior:**
- In split-screen mode, plays the sound **twice** (once per viewport, panned to -10000 and +10000)
- Second play uses slot offset `+0x2C` (44) for the duplicate range
- Volume attenuates with 2D distance from listener camera
- Applies the same Doppler-like pitch shift as the engine mixer
- Volume/distance formula identical to the engine mixer spatial code

---

## 5. Tire Screech / Skid Sound

### 5.1 Skid Sound (SkidBit.wav, Slot 0x13)

The skid/screech sound for the **viewer vehicle** is managed in `UpdateVehicleAudioMix` as part of the horn/special sound section:

```c
if (horn_trigger[viewport] > 0 && skid_playing[viewport] == 0 && race_end == 0) {
    DXSound::Play(viewport_offset + 0x12, 0, 0x5622, pan);  // Start skid
    skid_playing[viewport] = 1;
}
if (horn_trigger[viewport] != 0 && race_end == 0) {
    vol = min(horn_trigger[viewport], 0x7F);
    DXSound::Modify(viewport_offset + 0x13, vol, 0x5622, pan);  // Update skid
}
if (horn_trigger[viewport] == 0 && skid_playing[viewport] != 0) {
    DXSound::Stop(viewport_offset + 0x13);  // Stop skid
    skid_playing[viewport] = 0;
}
```

`DAT_004c3de0` acts as the skid intensity, driven by the vehicle dynamics lateral slip accumulator (`actor+0x18E` / `actor+0x190`).

### 5.2 Tire Visual Effects (Not Audio)

`UpdateFrontWheelSoundEffects` (0x43f420) and `UpdateRearWheelSoundEffects` (0x43f600) are **misnamed** -- they handle tire track mark rendering and smoke particle spawning, NOT audio. They:
- Acquire tire track emitters via `AcquireTireTrackEmitter`
- Spawn smoke via `SpawnVehicleSmokeVariant`
- Manage the tire track intensity byte at `DAT_004c3725[emitter*7]`

Thresholds:
- Front wheels: lateral force >= 0x3A99 (15001) triggers marks
- Rear wheels: lateral force >= 0x2711 (10001) triggers marks
- Smoke spawns when force >= 0x5000 above threshold
- Surface types 2/5/6/4/9 halve the tire mark intensity (softer surfaces)

---

## 6. Police Siren / Tracked Vehicle Audio

### 6.1 Siren Sound Trigger

`UpdateVehicleLoopingAudioState` (0x440a30):
- When `gWantedModeEnabled != 0` and the current actor is the cop (`DAT_004bf51c`):
  - Activates the tracked vehicle audio layer: `gTrackedVehicleAudioActive = 1`, fade target = 0x1000
  - Calls `AdvanceGlobalSkyRotation()` (visual effect for wanted mode)
  - Sets `DAT_004c3880 = 1` (siren active flag)

### 6.2 Siren Playback

In `UpdateVehicleAudioMix`, the tracked vehicle audio section plays on slots **0x14/0x15** (Siren3/Siren5) with spatial positioning relative to the cop vehicle:
- Distance attenuation halved compared to engine sounds (`* 0.5`)
- Base frequency: 0x5622 (22050 Hz)
- Doppler pitch shift applied
- Fade: instant binary (0x1000 = full, 0 = off)

### 6.3 Siren Fade

`StopTrackedVehicleAudio` (0x440ae0): Sets `gTrackedVehicleAudioFadeTarget = 0`, causing the mixer to stop the siren and clear the active flag next frame.

The fade-out path in the mixer:
```c
if (gTrackedVehicleAudioActive && !DAT_004c38b8 && gTrackedVehicleAudioFadeTarget == 0) {
    DXSound::Stop(0x15); DXSound::Stop(0x16);
    DXSound::Stop(0x41); DXSound::Stop(0x42);  // split-screen duplicates
    gTrackedVehicleAudioActive = 0;
}
```

---

## 7. Rain Ambient Sound

Slot 0x12 (Rain.wav) is loaded as a looping buffer. It is played/managed as part of the ambient sound set but its trigger conditions are integrated with the weather system. The rain sound activates when weather type indicates rain (weather type 2), sharing the same environmental configuration as the rain particle system.

---

## 8. CD Audio / Music System

### 8.1 Race Music Start

In `InitializeRaceSession` (0x42aa10), near the end of initialization:

```c
DXSound::CDPlay(DAT_00465e14 + 2, 1);  // Play CD track, looping
```

`DAT_00465e14` is the **CD track index offset** (likely selected per-track or per-race-type). The `+2` skips the data track and copyright track on the game CD.

### 8.2 Music Volume Control

From `ScreenSoundOptions` (0x41ea90):

| Button Index | Control | Global | Formula |
|---|---|---|---|
| 0 | SFX Mode | `DAT_00465fe8` | Cycles 0-1 (no 3D) or 0-2 (with 3D). Calls `DXSound::SetPlayback(mode)` |
| 1 | SFX Volume | `DAT_00465fec` | 0-100%. `DXSound::SetVolume((pct * 0xFC00) / 100 & 0xFC00)` |
| 2 | Music Volume | `DAT_00465ff0` | 0-100%. `DXSound::CDSetVolume((pct * 0xFC00) / 100 & 0xFC00)` |
| 3 | Music Test | -- | Navigates to screen 0x13 (music test screen) |
| 4 | OK | -- | Returns to Options Hub (screen 0x0C) |

**Volume conversion:** `raw_volume = (percentage * 64512) / 100`, masked to 0xFC00 alignment. This maps 0-100% to 0-64512 in DXSound's volume scale.

### 8.3 In-Race Audio Options Overlay

`RunAudioOptionsOverlay` (0x43bf70) provides a **pause-menu overlay** with 3 sliders:
- Row 0: `DAT_004b135c` -- mapped to `DAT_00466ea8` (some audio parameter, persists to config)
- Row 1: `DAT_004b1360` -- CD volume (float 0.0-1.0)
- Row 2: `DAT_004b1364` -- SFX volume (float 0.0-1.0)

Volume reads: `CDGetVolume() * 1.5500993e-05` and `GetVolume() * 1.5500993e-05` convert DXSound integer volumes to 0.0-1.0 floats. The constant `1.5500993e-05` = approximately `1/64512`.

Adjustments: +-0.02 per D-pad tick, clamped to [0.0, 1.0].

On exit:
- "OK" (row 3): Restores DXInput config, unmutes all
- "Quit" (row 4): Triggers race fade-out, force-feedback stop, unmutes all

### 8.4 Music Transitions

- **Menu -> Race:** `DXSound::CDPlay` is called in `InitializeRaceSession`, starting the race music track
- **Race -> Menu:** `ReleaseRaceSoundChannels` (called during fade-out at `local_10 == 255.0`) stops all race audio. The frontend then loads its own sound set via `LoadFrontendSoundEffects`
- **Pause:** `DXSound::MuteAll()` mutes everything; `DXSound::UnMuteAll()` restores on unpause

---

## 9. UpdateVehicleEngineSpeedSmoothed (0x42ed50) -- RPM Smoothing

This function smooths the raw engine speed toward a target derived from the vehicle's max RPM and throttle:

```c
max_rpm = *(short*)(vehicle_tuning + 0x72);  // From carparam.dat
target = (max_rpm - 400) * throttle_position / 256 + 400;

// Clamp: if in neutral/no throttle, target = 400 (idle)
if (no_throttle || throttle < 0) target = 400;

// Smoothed approach: delta = (target - current) / 16
delta = (target - current_speed) >> 4;

// Rate-limited:
if (delta > 0 && delta > 400) delta = 400;   // Max accel: +400/frame
if (delta < 0 && delta < 200) delta = 200;   // Max decel: +200/frame (less aggressive)

current_speed += delta;  // Stored at actor+0x310
```

The smoothed speed at `actor+0x310` is what the audio mixer reads for pitch calculation.

---

## 10. Traffic Vehicle Audio

For traffic actors (indices 6+), `UpdateVehicleAudioMix` manages engine loops at slots `0x25 + (index - 6)` (37+):

```c
speed_scaled = (raw_speed / 4) + 0x800;  // +2048 base offset for traffic
volume = clamp(speed_scaled, 0, 0xFFF) >> 5;  // 0-127

// Same pitch formula as race vehicles:
pitch = ((rand() % 100 + raw_speed) * 103) / 35 + 10000;
```

Traffic vehicles use the same spatial audio as race vehicles (distance attenuation + Doppler).

---

## 11. Sound Globals Reference

| Address | Type | Name | Description |
|---|---|---|---|
| `0x004c3770` | int[12] | Engine state P1 | Per-vehicle engine loop state (99=stopped, 1=drive, 2=rev) |
| `0x004c37a0` | int[12] | Traffic engine state | Per-traffic-vehicle engine state |
| `0x004c37dc` | int[12] | Horn/siren state | Per-vehicle tracked-audio state |
| `0x004c382c` | ptr[6] | Reverb flag | Non-null = use Reverb.wav instead of Rev.wav |
| `0x004c3810` | int[6] | Listener pos P1 | Camera world position (X,Y,Z) for P1 |
| `0x004c381c` | int[6] | Listener pos P2 | Camera world position for P2 |
| `0x004c3828` | int* | Active listener ptr | Points to current listener position |
| `0x004c3848` | int[12] | Horn playing state | Per-vehicle horn playback state |
| `0x004c3878` | int | Reverb actor idx | Index of actor using Reverb.wav |
| `0x004c387c` | int* | Listener velocity ptr | Points to current listener velocity |
| `0x004c3880` | int | Siren active flag | 1 = cop siren should be playing |
| `0x004c3888` | int[6] | Listener vel P1 | Listener velocity delta for P1 |
| `0x004c38a0` | int[6] | Gear state | Per-vehicle gear state for horn volume |
| `0x004c38b8` | int | Siren fade level | Current siren volume (0 or 0x1000) |
| `0x004c38c0` | int[6] | Prev listener pos | Previous frame listener position |
| `0x004c38d8` | byte | Viewport audio state | Tracks viewport mode changes |
| `0x004c3de0` | int[2] | Skid intensity | Per-viewport skid sound trigger level |
| `0x004c3de8` | int | Race end flag | Suppresses new sounds during fade-out |
| `0x004c3768` | int[2] | Skid playing flag | Per-viewport skid sound active |
| `0x00465e14` | int | CD track offset | Base CD audio track number for race |
| `0x00465fe8` | int | SFX mode | 0=mono, 1=stereo, 2=3D |
| `0x00465fec` | int | SFX volume % | 0-100 |
| `0x00465ff0` | int | Music volume % | 0-100 |
| `0x00474a00` | ptr[19] | Ambient WAV table | Pointers to ambient sound WAV filenames |
| `0x00474a4c` | ptr[3] | Engine variant table | Engine0.wav, car.wav, diesel.wav |
| `0x00474a58` | float | Audio distance scale | ~0.001817, world-to-audio-unit conversion |

---

## 12. Key Functions Reference

| Address | Name | Purpose |
|---|---|---|
| `0x00414640` | `LoadFrontendSoundEffects` | Loads 10 frontend WAVs from Sounds.zip into slots 1-10 |
| `0x0041ea90` | `ScreenSoundOptions` | Frontend sound options screen (SFX mode, volumes, music test) |
| `0x0042ed50` | `UpdateVehicleEngineSpeedSmoothed` | RPM smoothing toward target, rate-limited |
| `0x0043bf70` | `RunAudioOptionsOverlay` | In-race pause-menu audio overlay (3 sliders + OK/Quit) |
| `0x0043f420` | `UpdateFrontWheelSoundEffects` | **Visual only** -- tire tracks + smoke for front wheels |
| `0x0043f600` | `UpdateRearWheelSoundEffects` | **Visual only** -- tire tracks + smoke for rear wheels |
| `0x00440a30` | `UpdateVehicleLoopingAudioState` | Cop siren activation trigger |
| `0x00440ab0` | `StartTrackedVehicleAudio` | Start siren fade-in |
| `0x00440ae0` | `StopTrackedVehicleAudio` | Start siren fade-out |
| `0x00440b00` | `UpdateVehicleAudioMix` | **MAIN** per-frame sound mixer (~800 lines) |
| `0x00441a80` | `LoadVehicleSoundBank` | Loads Drive/Rev(Reverb)/Horn for one vehicle |
| `0x00441c60` | `LoadRaceAmbientSoundBuffers` | Loads 19 ambient WAVs + traffic engines from SOUND.ZIP |
| `0x00441d50` | `ReleaseRaceSoundChannels` | Race shutdown: stops 44-87, removes 0-43 |
| `0x00441d90` | `PlayVehicleSoundAtPosition` | Spatial one-shot sound with random variant selection |
| `0x00443280` | `LoadRaceVehicleAssets` | Vehicle model + sound bank loader (calls LoadVehicleSoundBank) |
| `0x00452e60` | `SetStreamVolume` | Streaming audio volume control |
| `0x004562c0` | `InitAudioPlayback` | DirectSound buffer creation for streaming |

---

## 13. Sound Flow Diagram

```
Race Init (InitializeRaceSession)
  |-- LoadRaceVehicleAssets
  |     |-- LoadVehicleSoundBank x6 (slots 0-17: Drive/Rev/Horn per vehicle)
  |-- LoadRaceAmbientSoundBuffers (slots 18-36: Rain/Skid/Siren/Scrape/Hits/Gear)
  |     |-- GetTrafficVehicleVariantType -> traffic engine slots 37+
  |-- DXSound::CDPlay(track+2, loop=1)  [start race music]
  |
Race Frame Loop (RunRaceFrame)
  |-- [Physics tick loop]
  |     |-- UpdateRaceActors -> UpdateVehicleActor
  |     |     |-- UpdatePlayerVehicleDynamics / UpdateAIVehicleDynamics
  |     |     |-- UpdateVehicleEngineSpeedSmoothed (RPM -> actor+0x310)
  |     |-- ResolveVehicleContacts
  |     |     |-- ApplyVehicleCollisionImpulse -> PlayVehicleSoundAtPosition
  |     |-- [Track contacts]
  |           |-- ApplyTrackSurfaceForceToActor -> PlayVehicleSoundAtPosition
  |
  |-- [Render pass - no sound calls]
  |
  |-- [Post-render sound update]
  |     |-- UpdateVehicleLoopingAudioState (horn/siren triggers)
  |     |-- UpdateVehicleAudioMix (engine loops, spatial mix, horn, skid, traffic, siren)
  |
  |-- RunAudioOptionsOverlay (if pause menu active)
  |
Race End (fade completes)
  |-- ReleaseRaceSoundChannels (stop all, remove all)
  |-- DXInput::StopEffects
```
