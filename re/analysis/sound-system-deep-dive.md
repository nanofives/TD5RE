# Sound System Deep Dive

**Date:** 2026-03-20
**Binaries:** M2DX.dll (Ghidra port 8193), TD5_d3d.exe (port 8195)
**Scope:** Complete end-to-end documentation of the DXSound subsystem in M2DX.dll and all EXE-side sound consumers.

---

## 1. Architecture Overview

TD5 uses a **two-tier sound architecture**:

1. **DXSound** (M2DX.dll) -- Low-level DirectSound wrapper with 44 base buffer slots, duplicate-buffer chaining for overlapping playback, WAV streaming, and CD audio via MCI or an optional archive-backed backend.
2. **TD5 Sound Manager** (TD5_d3d.exe) -- Game-level layer that loads per-vehicle sound banks, environmental ambience, and frontend SFX; computes per-frame spatial audio, Doppler pitch shift, and engine RPM-to-pitch mapping.

All DXSound methods are **static `__cdecl`** exports (no vtable, no object instance). State is held in DLL-static globals.

---

## 2. DXSound Internal Data Structures

### 2.1 Sound Buffer Table (`g_soundBufferTable`)

**Address:** `0x1005f974` (M2DX.dll)
**Stride:** 0x30 bytes (48 bytes = 12 DWORDs) per slot
**Capacity:** 44 base slots (indices 0-43) + 44 duplicate slots (indices 44-87) = 88 total

Each slot occupies 0x30 bytes with this layout:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x00 | 4 | `pSoundBuffer` | IDirectSoundBuffer* (base or duplicate) |
| +0x04-0x30 | ... | (metadata fields) | Packed per-slot state |
| +0x34 | 2 | `wFormatTag` | WAVE format tag (1 = PCM) |
| +0x36 | 2 | `nChannels` | Channel count |
| +0x38 | 4 | `nSamplesPerSec` | Sample rate (e.g., 22050) |
| +0x3C | 4 | `nAvgBytesPerSec` | Byte rate |
| +0x40 | 2 | `nBlockAlign` | Block alignment |
| +0x42 | 2 | `wBitsPerSample` | Bits per sample (8 or 16) |
| +0x44 | 2 | `cbSize` | Extra format bytes (always 0) |
| +0x4C | 4 | `freqSaved` | Cached playback frequency for Modify/Play |
| +0x50 | 4 | `panSaved` | Cached pan value |
| +0x54 | 4 | `nextDuplicateSlot` | Linked-list: next duplicate clone index (0 = end) |
| +0x58 | 4 | `isPlaying` | Set to 1 when Play(slot,v,f,p) starts |
| +0x5C | 4 | `loopFlag` | 0 = one-shot, nonzero = looping |

**Addressing:** The code indexes slots as `&g_soundBufferTable[slot * 12]` (12 DWORDs = 0x30 bytes). The IDirectSoundBuffer pointer is at DWORD index 0 of each slot.

### 2.2 Volume Attenuation Table (`g_soundVolumeAttenuationTable`)

**Address:** `0x10060c70` (M2DX.dll)
**Size:** 16 entries (computed at init)
**Formula:** `table[i] = -(int)ftol(log_scale(i))` -- negative hundredths-of-dB values for IDirectSoundBuffer::SetVolume

The table maps logical volume indices (0-15) to DirectSound attenuation values (0 to -10000 centibels). `SetVolume` indexes this table as `table[raw_volume >> 9]`, giving 16 volume tiers from 0-64512 input range.

### 2.3 Duplicate Count Table (`g_soundDuplicateCountTable`)

Parallel array tracking how many `DuplicateSoundBuffer` clones exist for each base slot. Used by `Play(slot)` to rotate through clones when the base buffer is already playing (polyphonic overlap). `Remove` walks the linked list via `nextDuplicateSlot` to release all clones.

### 2.4 Free Duplicate Slot Counter (`g_freeDuplicateBufferSlotCount`)

Tracks remaining available duplicate slots. Starts at 44 (0x2C). Decremented on each successful `DuplicateSoundBuffer` call. Prevents over-allocation.

---

## 3. DXSound API -- Full Function Documentation

### 3.1 Environment (0x1000cda0) -- Static Initialization

```
int __cdecl DXSound::Environment(void)
```

Zeroes the entire DXSound state block (0x46F DWORDs = 4540 bytes starting at `g_pDirectSound`). Then computes the 16-entry volume attenuation table using a logarithmic scale. Sets `g_isSoundPanEnabled = 1`.

Called once at DLL load time. No DirectSound objects are created here.

### 3.2 Create (0x1000ce30) -- DirectSound Bootstrap

```
int __cdecl DXSound::Create(void)
```

**Flow:**
1. If already initialized (`g_isSoundInitialized != 0`), returns 1 immediately.
2. Calls `DirectSoundCreate(NULL, &g_pDirectSound, NULL)`. On failure, logs "No Sound Card Found" and falls through.
3. Sets cooperative level to DSSCL_PRIORITY via `IDirectSound::SetCooperativeLevel(hWnd, 1)`.
4. Creates primary sound buffer with `DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME` (flags = 0x81, size struct = 0x14).
5. Starts the primary buffer with `IDirectSoundBuffer::Play(0, 0, DSBPLAY_LOOPING)`.
6. Initializes CD audio backend: calls `InitializeCDAudioBackendFromInstallSource`. If successful, promotes `g_pCDAudioBackendInitCandidate` to `g_pCDAudioBackend`.
7. Sets `g_isSoundInitialized = 1`, `g_isStreamPlaying = 0`.

**Returns:** 1 on success or if no sound card (graceful degradation), 0 only on primary buffer creation/play failure.

### 3.3 Destroy (0x1000cfd0) -- Full Teardown

```
int __cdecl DXSound::Destroy(void)
```

1. If streaming is active, stops the stream buffer, frees `g_pStreamFileBuffer`, calls `Remove` on the stream slot.
2. Calls `Remove(i)` for all 44 base slots (0-43), releasing all buffers and their duplicate chains.
3. Releases primary sound buffer and DirectSound object.
4. Clears `g_isSoundInitialized` and `g_isStreamPlaying`.

### 3.4 Load (0x1000d0a0 / 0x1000d0c0) -- File-Based WAV Loading

```
int __cdecl DXSound::Load(char* filename, int slot, int loopFlag)              // 3-arg: delegates with nDuplicates=1
int __cdecl DXSound::Load(char* filename, int slot, int loopFlag, int nDuplicates)  // 4-arg: full
```

**Flow (4-arg version):**
1. Validates `slot < 44` (0x2C). Logs error if exceeded.
2. Opens the WAV file via `DX::FOpen`, reads entire contents into a temporary buffer.
3. Calls `CreateSoundBufferFromWaveImage(buffer, slot, 0)` to parse RIFF/WAVE and create the DirectSound buffer.
4. Frees the temporary buffer.
5. If `nDuplicates > 1`, iterates to create duplicate clones:
   - Finds a free slot by scanning from index 44 (0x2C) upward for null buffer pointers.
   - Calls `IDirectSound::DuplicateSoundBuffer(base, &clone)`.
   - Links clones into a singly-linked list via `nextDuplicateSlot`.
   - Stores `loopFlag` in each clone's metadata.
   - Decrements `g_freeDuplicateBufferSlotCount`.
6. Stores `loopFlag` in the base slot's metadata.

**The 3-arg overload** simply calls the 4-arg version with `nDuplicates = 1`.

### 3.5 LoadBuffer (0x1000d240 / 0x1000d260) -- In-Memory WAV Loading

```
int __cdecl DXSound::LoadBuffer(void* waveData, int slot, int loopFlag)
int __cdecl DXSound::LoadBuffer(void* waveData, int slot, int loopFlag, int nDuplicates)
```

Identical to `Load` but skips the file I/O -- takes a pointer to an already-loaded RIFF/WAVE image in memory. This is the primary path used by the EXE (loads from ZIP archives into memory first, then passes the buffer here).

### 3.6 LoadComplete (0x1000d370) -- No-Op Stub

```
int __cdecl DXSound::LoadComplete(void)
```

Returns 1. Shared ordinal with `DXInput::ReadClose`. No functional purpose -- likely a pipeline synchronization hook that was never implemented.

### 3.7 Play -- Two Overloads

#### Play(slot) (0x1000d380) -- Simple Playback

```
int __cdecl DXSound::Play(int slot)
```

1. Gets the buffer pointer from `g_soundBufferTable[slot * 12]`.
2. Checks `g_pDirectSound != 0`, buffer is non-null, and `g_isSoundMuted == 0`.
3. Calls `IDirectSoundBuffer::GetStatus(&status)`.
4. If buffer is already playing (`status == 1`) and duplicates exist, walks the duplicate chain to find a non-playing clone. Stops and resets the first available clone.
5. Calls `IDirectSoundBuffer::Play(0, 0, loopFlag)` where `loopFlag` is 0 (one-shot) or 1 (DSBPLAY_LOOPING) from the slot metadata.
6. **Returns:** `status + 1` on success (nonzero = "handle"), 0 on failure. The EXE uses this return value as a **play handle** for subsequent `Modify`/`ModifyOveride` calls.

#### Play(slot, volume, frequency, pan) (0x1000d470) -- Parameterized Playback

```
int __cdecl DXSound::Play(int slot, int volIndex, int frequency, int pan)
```

1. If `g_isSoundPanEnabled == 0`, forces `pan = 0`.
2. Stops any current playback on the buffer, resets cursor to 0.
3. Starts playback (looping or one-shot per slot metadata).
4. Sets volume via `IDirectSoundBuffer::SetVolume(g_soundVolumeAttenuationTable[volIndex])`.
5. Sets frequency via `IDirectSoundBuffer::SetFrequency(frequency)`.
6. Sets pan via `IDirectSoundBuffer::SetPan(pan)`.
7. Caches frequency and pan in slot metadata. Sets `isPlaying = 1`.
8. **Returns:** `volIndex + 1` as play handle, or 0 on failure.

### 3.8 ModifyOveride (0x1000d560) -- Parameter Update (Ignores Mute)

```
int __cdecl DXSound::ModifyOveride(int playHandle, int volIndex, int frequency, int pan)
```

**Key difference from Modify:** Always applies volume, even when `g_isSoundMuted == 1`. Used by the EXE for in-race audio options preview (e.g., horn test).

- `playHandle` is 1-indexed (the value returned by `Play`). Internally converts: `slotIndex = (playHandle * 3) - 3` then indexes by `* 4`.
- Updates volume from `g_soundVolumeAttenuationTable[volIndex]`, frequency, and pan.
- Caches all values in slot metadata.

### 3.9 Modify (0x1000d5f0) -- Parameter Update (Respects Mute)

```
int __cdecl DXSound::Modify(int playHandle, int volIndex, int frequency, int pan)
```

Same as `ModifyOveride` except:
- Volume write is **gated** by `g_isSoundMuted == 0`. If muted, frequency and pan are still updated but volume is skipped.
- This is the standard path for per-frame engine sound updates.

### 3.10 Remove (0x1000d690) -- Buffer Release

```
int __cdecl DXSound::Remove(int slot)
```

1. If slot has a buffer, walks the duplicate chain (via `nextDuplicateSlot`), calling `IDirectSoundBuffer::Release()` on each clone and zeroing the slot.
2. Releases the base buffer.
3. Zeroes `g_soundDuplicateCountTable[slot]`.

### 3.11 Stop (0x1000d730) -- Stop Playback

```
int __cdecl DXSound::Stop(int slot)
```

**Note:** `slot` parameter is **1-indexed** (play handle convention). Internally accesses `g_soundBufferTable[(slot - 1) * 12]`.

Calls `IDirectSoundBuffer::Stop()`. Returns 1 on success, 0 on failure.

### 3.12 Status (0x1000d770) -- Query Playing State

```
int __cdecl DXSound::Status(int slot)
```

**Also 1-indexed.** Calls `IDirectSoundBuffer::GetStatus(&status)`. Returns 1 if playing, 0 otherwise. Gated by `g_isSoundMuted` -- always returns 0 when muted.

### 3.13 SetVolume / GetVolume (0x1000d850 / 0x1000d890) -- Master Volume

```
int __cdecl DXSound::SetVolume(int volume)    // 0-64512 range
int __cdecl DXSound::GetVolume(void)          // returns cached value
```

`SetVolume` applies to the **primary sound buffer** (global master volume). Indexes the attenuation table as `table[volume >> 9]`, giving 16 volume steps. Caches the raw value in `g_masterSoundVolume`.

`GetVolume` returns the cached raw value (NOT the DirectSound centibel value).

### 3.14 MuteAll / UnMuteAll (0x1000d7c0 / 0x1000d800)

```
int __cdecl DXSound::MuteAll(void)
int __cdecl DXSound::UnMuteAll(void)
```

**MuteAll:** Iterates all 88 slots (stride 0x30). For each non-null buffer, sets volume to -10000 centibels (0xFFFFD8F0 = silence). Sets `g_isSoundMuted = 1`.

**UnMuteAll:** Iterates all 88 slots. For each non-null buffer, restores volume from `g_soundVolumeAttenuationTable[slot.volumePresetIndex]`. Clears `g_isSoundMuted = 0`.

### 3.15 SetPlayback (0x1000d090)

```
int __cdecl DXSound::SetPlayback(int enabled)
```

Sets `g_isSoundPanEnabled`. When 0, all `Play`/`Modify`/`ModifyOveride` calls force pan to 0 (centered).

### 3.16 GetDSObject (0x1000d070)

```
int __cdecl DXSound::GetDSObject(void** ppDirectSound, void** ppPrimaryBuffer)
```

Returns raw DirectSound and primary buffer pointers. Used by the EXE's streaming audio subsystem to create its own buffers.

---

## 4. WAV Parsing and Buffer Creation

### 4.1 CreateSoundBufferFromWaveImage (0x1000dfd0)

```
void __cdecl CreateSoundBufferFromWaveImage(int* waveData, int slot, int isStreaming)
```

Parses a RIFF/WAVE memory image:

1. Validates RIFF header (`0x46464952`) and WAVE tag (`0x45564157`).
2. Iterates chunks:
   - **`fmt ` chunk** (`0x20746d66`): Validates `wFormatTag == 1` (PCM only). Copies `nChannels`, `nSamplesPerSec`, `nAvgBytesPerSec`, `nBlockAlign`, `wBitsPerSample` into the slot's WAVEFORMATEX at offset +0x34.
   - **`data` chunk** (`0x61746164`): Records the data pointer and size. If `isStreaming != 0`, overrides size to `g_streamHalfBufferBytes * 2` (the double-buffer size).
3. If both `fmt` and `data` chunks found (bitmask == 3):
   - Fills a `DSBUFFERDESC` struct: size = 0x14, flags = 0xEA (`DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_STATIC`).
   - Calls `IDirectSound::CreateSoundBuffer(&desc, &slot.pSoundBuffer, NULL)`.
   - Calls `WriteToSoundBuffer` to copy audio data.
   - For streaming: seeks the file handle back by `g_streamHalfBufferBytes` to prepare for the first refill.

### 4.2 WriteToSoundBuffer (0x1000e320)

```
int __cdecl WriteToSoundBuffer(IDirectSoundBuffer* pBuffer, void* srcData, int length, int whichHalf)
```

1. Computes lock offset: `offset = whichHalf ? g_streamHalfBufferBytes : 0`.
2. Calls `IDirectSoundBuffer::Lock(offset, length, &ptr1, &len1, &ptr2, &len2, 0)`.
3. On `DSERR_BUFFERLOST`, calls `IDirectSoundBuffer::Restore()` then retries the Lock.
4. Copies `srcData` into `ptr1` (and wraps into `ptr2` if the buffer wraps around).
5. Calls `IDirectSoundBuffer::Unlock(ptr1, len1, ptr2, len2)`.

---

## 5. Streaming Audio System

### 5.1 Architecture

DXSound implements a **double-buffered ping-pong** streaming system for playing WAV files larger than can fit in memory (e.g., music tracks).

**Key globals:**

| Global | Description |
|--------|-------------|
| `g_streamSoundSlot` | DXSound slot index used by the stream |
| `g_streamFileHandle` | `DX::FOpen` handle to the open WAV file |
| `g_pStreamFileBuffer` | Heap buffer, size = `g_streamHalfBufferBytes * 2` (0x81600 = 530,944 bytes) |
| `g_streamHalfBufferBytes` | Half-buffer size = 0x40B00 = 265,472 bytes (~6 seconds at 22050 Hz 16-bit stereo) |
| `g_streamBufferHalfState` | Ping-pong state: 1 = first half playing, 2 = second half playing |
| `g_streamReadCompletionState` | 0 = reading, 1 = file exhausted (one more refill), 2 = fully done |
| `g_streamStopCursor` | Play cursor position at which to stop (set when file runs out) |
| `g_streamPlayCursor` | Sampled via `IDirectSoundBuffer::GetCurrentPosition` |
| `g_isStreamPlaying` | Master stream-active flag |

### 5.2 PlayStream (0x1000dd50) -- Start Streaming

```
int __cdecl DXSound::PlayStream(char* filename, int slot)
```

1. If a stream is already playing, stops and cleans up the old stream.
2. Opens the WAV file via `DX::FOpen(filename, 0)`.
3. Allocates the staging buffer: `g_pStreamFileBuffer = DX::Allocate(0x81600)`.
4. Sets `g_streamHalfBufferBytes = 0x40B00`.
5. Pre-reads both halves: `DX::FRead(buffer, halfSize * 2, handle)`.
6. Creates the DirectSound buffer from the first half via `CreateSoundBufferFromWaveImage(buffer, slot, isStreaming=1)`.
7. Resets cursor to 0, starts looping playback.
8. Sets `g_isStreamPlaying = 1`.

### 5.3 Refresh (0x1000ded0) -- Per-Frame Streaming Tick

```
int __cdecl DXSound::Refresh(void)
```

Called every frame by the game loop. Implements the double-buffer refill logic:

1. Reads `g_streamPlayCursor` via `IDirectSoundBuffer::GetCurrentPosition`.
2. **End-of-file detection:** If `g_streamReadCompletionState == 2` (fully padded) and play cursor has passed `g_streamStopCursor`, tears down the stream (stops buffer, frees memory, removes slot).
3. **Ping-pong refill:**
   - State 1 (first half playing): When cursor enters second half (`cursor >= halfSize`), refills the **second half** via `RefillStreamingBufferHalf(1)`. Transitions to state 2.
   - State 2 (second half playing): When cursor wraps back to first half (`cursor < halfSize`), refills the **first half** via `RefillStreamingBufferHalf(2)`. Transitions to state 1.

### 5.4 RefillStreamingBufferHalf (0x1000e1a0)

```
void __cdecl RefillStreamingBufferHalf(int whichHalf)
```

1. If `g_streamReadCompletionState == 0` (still reading), reads `g_streamHalfBufferBytes` from the file.
2. If read returned fewer bytes than requested (EOF):
   - Pads the remainder with **silence** (0x00 for 16-bit, 0x80 for 8-bit -- the PCM zero-crossing point).
   - Closes the file handle.
   - Computes `g_streamStopCursor` = bytes-read offset (adjusting for which half was being filled).
   - Sets `g_streamReadCompletionState = 1` (first pass done).
3. If `g_streamReadCompletionState` was already 1 (second pass after EOF), sets it to 2 (fully done -- next Refresh will stop).
4. Uploads the buffer via `WriteToSoundBuffer(pBuffer, staging, halfSize, whichHalf)`.
5. On write failure, releases the sound buffer (cleanup).

### 5.5 StopStream (0x1000de70)

```
int __cdecl DXSound::StopStream(int slot)
```

Stops the buffer, frees `g_pStreamFileBuffer`, calls `Remove(slot)`, clears `g_isStreamPlaying`.

---

## 6. CD Audio System

### 6.1 Dual Backend Architecture

DXSound supports two CD audio backends:

1. **Archive-backed backend** (`g_pCDAudioBackend`): COM-like object initialized by `InitializeCDAudioBackendFromInstallSource`. Uses a vtable with methods at offsets +0x0C (bind DirectSound), +0x10 (open archive), +0x14 (play), +0x18 (stop), +0x1C (pause), +0x20 (resume), +0x30 (set volume). Reads from `TD5_Track*.Wv2` archives on the install source path (registry key `Software\Microsoft\Windows\CurrentVersion\InstallSource`). Format: wv2wav (compressed audio).

2. **MCI fallback**: Standard Windows MCI `cdaudio` device. Used when the archive backend is unavailable (no CD in drive, missing files).

### 6.2 CDPlay (0x1000d8a0)

```
int __cdecl DXSound::CDPlay(int trackNumber, int shouldLoop)
```

**Archive backend path:** Calls `vtable[+0x14](0, trackNumber, shouldLoop != 0)`. Returns the boolean result.

**MCI fallback path:**
1. If `g_isCdAudioOpen == 0`, opens the MCI cdaudio device:
   - `MCI_OPEN` (0x803) with device type "cdaudio".
   - `MCI_SET` (0x80D) with time format = MCI_FORMAT_TMSF (10).
   - `MCI_STATUS` (0x814) to get total track count -> `g_cdLastTrackNumber`.
2. Sets MCI play parameters: `from = trackNumber`, `to = trackNumber + 1`.
3. If playing the last track, uses `MCI_PLAY | MCI_NOTIFY` (flags = 5); otherwise `MCI_PLAY | MCI_FROM | MCI_TO | MCI_NOTIFY` (flags = 0xD).
4. On first successful play, enumerates auxiliary audio devices via `auxGetNumDevs()`/`auxGetDevCapsA()` to find a `AUXCAPS_CDAUDIO` device for volume control. Stores in `g_cdAuxDeviceId`.
5. Caches `g_cdReplayTrack = shouldLoop ? trackNumber : 0` for CDReplay.

### 6.3 CDStop (0x1000dae0)

```
int __cdecl DXSound::CDStop(void)
```

Clears `g_cdReplayTrack`. On archive backend: calls `vtable[+0x18]()`. On MCI: sends `MCI_STOP` (0x808) with `MCI_WAIT` flag, restores AUX volume, closes the MCI device (`MCI_CLOSE` 0x804), clears `g_isCdAudioOpen`.

### 6.4 CDReplay (0x1000dc10)

```
int __cdecl DXSound::CDReplay(void)
```

If `g_cdReplayTrack > 0`, calls `CDPlay(g_cdReplayTrack, 1)`. Used to auto-loop CD tracks.

### 6.5 CDPause / CDResume (0x1000dcd0 / 0x1000dd10)

```
int __cdecl DXSound::CDPause(void)
int __cdecl DXSound::CDResume(void)
```

**CDPause:** Archive backend -> `vtable[+0x1C]()`. MCI -> `MCI_PAUSE` (0x809).

**CDResume:** Archive backend -> `vtable[+0x20]()`. MCI -> calls `CDPlay(g_cdReplayTrack, 1)` (restarts the track rather than true resume).

### 6.6 CDSetVolume / CDGetVolume (0x1000dc30 / 0x1000dcc0)

```
int __cdecl DXSound::CDSetVolume(int volume)    // 0-64512
int __cdecl DXSound::CDGetVolume(void)
```

**CDSetVolume:** Converts to Windows DWORD volume format: if `volume < 0x10000`, expands to `volume * 0x10001` (duplicates into high/low words for L/R channels).
- Archive backend: extracts byte `(volume >> 8) & 0xFF`, converts to float `* 0.003968254` (= 1/252), calls `vtable[+0x30](floatVol)`.
- MCI fallback: calls `auxSetVolume(g_cdAuxDeviceId, dwordVolume)`.

Caches raw value in `g_cdVolume`.

### 6.7 InitializeCDAudioBackendFromInstallSource (0x10013000)

Creates an `ArchiveProviderManager` COM object, acquires:
- Class ID `0x6000` -> CD audio backend candidate.
- Class ID `0x1005` -> Registry/config provider.

Reads `HKLM\Software\Microsoft\Windows\CurrentVersion\InstallSource` from the registry, appends `TD5_Track*.Wv2*` path, and calls the backend's Open method with format `"wv2wav"`. On success, the candidate is promoted to `g_pCDAudioBackend` in `Create`.

---

## 7. EXE-Side Sound Integration

### 7.1 Slot Map Summary

| Slot Range | Count | Content | Source |
|------------|-------|---------|--------|
| 0-17 | 18 | Vehicle sounds (6 vehicles x 3: Drive/Rev/Horn) | Per-vehicle ZIP archives |
| 18-36 | 19 | Ambient/effects (Rain, Skid, Sirens, Scrape, Hits, Gear) | SOUND\SOUND.ZIP |
| 37+ | var | Traffic engine loops (Engine0/car/diesel.wav) | SOUND\SOUND.ZIP |
| 44-87 | 44 | Duplicate buffers (overlapping playback, split-screen) | DuplicateSoundBuffer |

**Frontend** (loaded separately, shares same slot range):
| Slot | Content | Source |
|------|---------|--------|
| 1-10 | Menu SFX (pings, whoosh, crash, uh-oh) | Front End\Sounds\Sounds.zip |

### 7.2 LoadVehicleSoundBank (0x441a80)

```
void __cdecl LoadVehicleSoundBank(LPCSTR archiveName, int vehicleIndex, char* reverbFlag)
```

Loads 3 WAV files from a vehicle's ZIP archive into consecutive sound slots:

| Slot Offset | File | Loop | Duplicates | Notes |
|-------------|------|------|------------|-------|
| `vehicleIndex * 3 + 0` | `Drive.wav` | Yes (1) | 2 | High-RPM engine loop |
| `vehicleIndex * 3 + 1` | `Rev.wav` or `Reverb.wav` | Yes (1) | 2 | If `reverbFlag != NULL`, loads `Reverb.wav` (local player proximity variant) |
| `vehicleIndex * 3 + 2` | `Horn.wav` | No (0) | 2 | Vehicle horn one-shot |

**On first call** (`vehicleIndex == 0`): zeroes all engine state arrays, horn state, tracked vehicle audio state.

Each file is: extracted from ZIP into memory -> passed to `DXSound::LoadBuffer` -> freed.

### 7.3 LoadRaceAmbientSoundBuffers (0x441c60)

Loads 19 ambient WAV files from `SOUND\SOUND.ZIP` into slots 18-36:

| Index | Slot | File | Loop |
|-------|------|------|------|
| 0 | 18 | Rain.wav | Yes |
| 1 | 19 | SkidBit.wav | Yes |
| 2 | 20 | Siren3.wav | Yes |
| 3 | 21 | Siren5.wav | Yes |
| 4 | 22 | ScrapeX.wav | No |
| 5 | 23 | Bottom3.wav | No |
| 6 | 24 | Bottom1.wav | No |
| 7 | 25 | Bottom4.wav | No |
| 8 | 26 | Bottom2.wav | No |
| 9 | 27 | HHit1.wav | No |
| 10 | 28 | HHit2.wav | No |
| 11 | 29 | HHit3.wav | No |
| 12 | 30 | HHit4.wav | No |
| 13 | 31 | LHit1.wav | No |
| 14 | 32 | LHit2.wav | No |
| 15 | 33 | LHit3.wav | No |
| 16 | 34 | LHit4.wav | No |
| 17 | 35 | LHit5.wav | No |
| 18 | 36 | Gear1.wav | No |

**Looping rule:** `loop = (index < 4)` -- first 4 entries (Rain, SkidBit, Siren3, Siren5) are looping; rest are one-shot.

**Traffic engine sounds:** For each traffic actor beyond the 6 race slots, loads an engine loop into slot `0x25 + (trafficIndex)`:

| Variant | File | Selection |
|---------|------|-----------|
| 0 | Engine0.wav | Generic/highway |
| 1 | car.wav | Standard car |
| 2 | diesel.wav | Truck (model ID 0x0E) |

`GetTrafficVehicleVariantType` (0x443f10) selects the variant from the NPC group table.

### 7.4 LoadFrontendSoundEffects (0x414640)

Loads 10 SFX from `Front End\Sounds\Sounds.zip` into slots 1-10:

| Slot | File | Purpose |
|------|------|---------|
| 1 | ping3.wav | Menu tick (low) |
| 2 | ping2.wav | Menu tick (mid) |
| 3 | Ping1.wav | Menu tick (high) |
| 4 | Crash1.wav | Menu crash/select |
| 5 | Whoosh.wav | Slide transition |
| 6 | Whoosh.wav | Slide transition (duplicate) |
| 7 | Crash1.wav | Confirm (duplicate) |
| 8 | Whoosh.wav | Slide transition (duplicate) |
| 9 | Crash1.wav | Crash (duplicate) |
| 10 | Uh-Oh.wav | Error/warning |

All loaded as non-looping, no duplicates. Uses `OpenArchiveFileForRead` instead of the race path's `GetArchiveEntrySize`/`ReadArchiveEntry` pair.

### 7.5 UpdateFrontWheelSoundEffects / UpdateRearWheelSoundEffects (0x43f420 / 0x43f600)

Despite their names, these functions handle **visual effects only** (tire track marks and smoke particles), NOT audio. They:
- Acquire tire track emitters via `AcquireTireTrackEmitter`.
- Spawn smoke particles via `SpawnVehicleSmokeVariant`.
- Manage tire track intensity bytes.

**Thresholds:**
- Front wheels: lateral force >= 15001 (0x3A99) triggers marks.
- Rear wheels: lateral force >= 10001 (0x2711) triggers marks.
- Smoke spawns when force exceeds threshold + 20480 (0x5000).
- Surface types 2, 4, 5, 6, 9 halve the mark intensity (softer/different surfaces).

---

## 8. Engine Sound System (RPM-to-Pitch)

### 8.1 RPM Smoothing (UpdateVehicleEngineSpeedSmoothed, 0x42ed50)

Raw engine speed is smoothed toward a target:

```
max_rpm = vehicle_tuning.max_rpm;           // carparam.dat offset 0x72
target = (max_rpm - 400) * throttle / 256 + 400;   // 400 = idle RPM
delta = (target - current) >> 4;            // 1/16th approach per frame
delta = clamp(delta, -200, +400);           // asymmetric rate limit
current += delta;                           // stored at actor+0x310
```

### 8.2 Pitch Calculation

```c
// Random jitter for natural variation
pitch = ((rand() % 100 + smoothed_rpm) * 103) / 35 + 10000;
// Simplifies to: pitch ~ rpm * 2.94 + 10000 + jitter
// Range: ~10,000 Hz (idle) to ~40,000 Hz (redline)
```

### 8.3 Volume Calculation

```c
speed_scaled = (smoothed_rpm / 4) + base_offset;
// base_offset: +0x800 for viewer vehicle, +0xC00 for tracked vehicle, +0x400 for others
volume = clamp(speed_scaled, 0, 0xFFF) >> 5;  // 0-127 range
```

### 8.4 Spatial Audio (Non-Viewer Vehicles)

```c
// 2D distance in audio units
dx = (listener.x - vehicle.x) * 0.001817;
dz = (listener.z - vehicle.z) * 0.001817;
distance = sqrt(dx*dx + dz*dz);
if (replay_mode) distance *= 0.5;

// Linear volume falloff
vol = (127 - (distance >> 7)) * volume / 127;

// Doppler pitch shift
v_vehicle = dot(vehicle.velocity, normalize(dx, dz));
v_listener = dot(listener.velocity, normalize(dx, dz));
doppler = clamp((v_vehicle * 4 + 4096) / (v_listener * 4 + 4096), 0.0, 2.0);
pitch_final = pitch * ((doppler - 1.0) * 0.074074 + 1.0);
// Effective range: +/- 7.4% pitch shift from Doppler
```

### 8.5 Engine Sound State Machine

Per vehicle, per viewport, a state byte tracks the active engine sample:

| State | Meaning | Action |
|-------|---------|--------|
| 99 | Stopped (initial) | First update starts appropriate loop |
| 1 | Drive.wav active | High-speed engine. Updated via `DXSound::Modify` each frame |
| 2 | Rev/Reverb.wav active | Low-speed/idle. Used when `rpm < 1000` and vehicle has reverb flag |

State transitions: when speed crosses 1000 RPM threshold, the old slot is stopped and the new one started.

---

## 9. Collision Sound Triggers

### 9.1 Vehicle-to-Vehicle (ApplyVehicleCollisionImpulse, 0x4079c0)

| Impulse Magnitude | Sound | Volume | Pitch | Variants |
|-------------------|-------|--------|-------|----------|
| >= 12,801 (0x3201) | LHit1-5 (slots 31-35) | Full (0x1000) | 3,330 Hz | 5 random |
| >= 51,201 (0xC801) | HHit1-4 (slots 27-30) | Full (0x1000) | 8,600 Hz | 4 random |

### 9.2 Wall/Barrier (ApplyTrackSurfaceForceToActor, 0x406980)

| Lateral Force | Sound | Volume | Pitch | Variants |
|---------------|-------|--------|-------|----------|
| 12,801 - 102,400 | ScrapeX (slot 22) | force/8, clamped 0x400-0x800 | 22,050 Hz | 1 |
| > 102,400 (0x19000) | HHit1-4 (slots 27-30) | force/8, clamped 0x400-0x800 | 8,600 Hz | 4 random |

### 9.3 PlayVehicleSoundAtPosition (0x441d90)

Spatial one-shot dispatcher used by both collision types:
- Random variant: `slot += rand() % numVariants`.
- Split-screen: plays twice (once per viewport with opposite pan offsets +/-10000).
- Duplicate range: second play uses slot + 44 (0x2C).
- Same distance/Doppler formulas as engine mixer.

---

## 10. Sound Flow Diagram

```
Game Boot
  |-- DXSound::Environment()        [zero state, build volume table]
  |-- DXSound::Create()             [DirectSoundCreate, primary buffer, CD backend init]

Frontend
  |-- LoadFrontendSoundEffects()    [slots 1-10 from Sounds.zip]
  |-- DXSound::Play(slot)           [menu SFX on button clicks]
  |-- DXSound::CDPlay(track, 1)     [menu music from CD/archive]

Race Init (InitializeRaceSession)
  |-- LoadVehicleSoundBank() x6     [slots 0-17: Drive/Rev/Horn per vehicle]
  |-- LoadRaceAmbientSoundBuffers() [slots 18-36: ambient + traffic engines 37+]
  |-- DXSound::CDPlay(track+2, 1)   [race music]

Race Frame Loop (RunRaceFrame)
  |-- [Physics tick]
  |     |-- UpdateVehicleEngineSpeedSmoothed()  [RPM -> actor+0x310]
  |     |-- ApplyVehicleCollisionImpulse()      [-> PlayVehicleSoundAtPosition]
  |     |-- ApplyTrackSurfaceForceToActor()     [-> PlayVehicleSoundAtPosition]
  |
  |-- [Post-render sound update]
  |     |-- UpdateVehicleLoopingAudioState()    [horn/siren triggers]
  |     |-- UpdateVehicleAudioMix()             [MAIN: engines, spatial, horn, skid, siren, traffic]
  |           |-- For each vehicle:
  |           |     |-- Compute RPM -> pitch (with jitter)
  |           |     |-- Compute distance -> volume attenuation
  |           |     |-- Compute Doppler -> pitch shift
  |           |     |-- DXSound::Modify(handle, vol, pitch, pan)
  |           |-- Siren: spatial Siren3/Siren5 on cop vehicle
  |           |-- Skid: SkidBit.wav on viewer lateral slip
  |           |-- Horn: viewer horn with gear-based volume
  |
  |-- DXSound::Refresh()                       [streaming buffer refill]

Race Pause
  |-- DXSound::MuteAll()                       [silence all buffers]
  |-- RunAudioOptionsOverlay()                 [volume sliders]
  |-- DXSound::UnMuteAll()                     [restore on unpause]

Race End
  |-- ReleaseRaceSoundChannels()               [Stop 44-87, Remove 0-43]
  |-- DXInput::StopEffects()

Game Exit
  |-- DXSound::Destroy()                       [release everything]
```

---

## 11. Key Addresses Reference

### M2DX.dll Globals

| Address | Type | Name |
|---------|------|------|
| `0x1005f974` | DWORD[88*12] | `g_soundBufferTable` (slot 0 at offset +0x00) |
| `0x10060c70` | int[16] | `g_soundVolumeAttenuationTable` |
| `0x10060a24` | int | `g_pDirectSound` (IDirectSound*) |
| `0x10060a28` | int | `g_pPrimarySoundBuffer` (IDirectSoundBuffer*) |
| `0x10060a2c` | int | `g_isSoundInitialized` |
| `0x10060a30` | int | `g_isSoundMuted` |
| `0x10060a34` | int | `g_isSoundPanEnabled` |
| `0x10060a38` | int | `g_isStreamPlaying` |
| `0x10060a3c` | int | `g_streamSoundSlot` |
| `0x10060a40` | void* | `g_pStreamFileBuffer` |
| `0x10060a44` | int | `g_streamHalfBufferBytes` (0x40B00 = 265,472) |
| `0x10060a48` | int | `g_streamBufferHalfState` (1 or 2) |
| `0x10060a4c` | int | `g_streamPlayCursor` |
| `0x10060a50` | int | `g_streamReadCompletionState` (0/1/2) |
| `0x10060a54` | int | `g_streamStopCursor` |
| `0x10060a58` | int | `g_streamFileHandle` |
| `0x10060a5c` | int | `g_cdDeviceId` (MCI device) |
| `0x10060a60` | int | `g_cdAuxDeviceId` |
| `0x10060cb0` | int | `g_masterSoundVolume` |
| `0x10060cb4` | int | `g_cdVolume` |
| `0x10060cb8` | int | `g_cdReplayTrack` |
| `0x10060cbc` | int | `g_cdLastTrackNumber` |
| `0x10060cc0` | int | `g_isCdAudioOpen` |
| `0x10060cc4` | int* | `g_pCDAudioBackend` |
| `0x10060cc8` | int* | `g_pCDAudioBackendInitCandidate` |

### M2DX.dll Functions

| Address | Ordinal | Signature |
|---------|---------|-----------|
| `0x1000cda0` | 53 | `DXSound::Environment()` |
| `0x1000ce30` | 31 | `DXSound::Create()` |
| `0x1000cfd0` | 42 | `DXSound::Destroy()` |
| `0x1000d070` | 74 | `DXSound::GetDSObject(void**, void**)` |
| `0x1000d090` | 149 | `DXSound::SetPlayback(int)` |
| `0x1000d0a0` | 105 | `DXSound::Load(char*, int, int)` |
| `0x1000d0c0` | 106 | `DXSound::Load(char*, int, int, int)` |
| `0x1000d240` | 107 | `DXSound::LoadBuffer(void*, int, int)` |
| `0x1000d260` | 108 | `DXSound::LoadBuffer(void*, int, int, int)` |
| `0x1000d370` | 109 | `DXSound::LoadComplete()` |
| `0x1000d380` | 124 | `DXSound::Play(int)` |
| `0x1000d470` | 125 | `DXSound::Play(int, int, int, int)` |
| `0x1000d560` | 118 | `DXSound::ModifyOveride(int, int, int, int)` |
| `0x1000d5f0` | 117 | `DXSound::Modify(int, int, int, int)` |
| `0x1000d690` | 136 | `DXSound::Remove(int)` |
| `0x1000d730` | 153 | `DXSound::Stop(int)` |
| `0x1000d770` | 152 | `DXSound::Status(int)` |
| `0x1000d7c0` | 119 | `DXSound::MuteAll()` |
| `0x1000d800` | 159 | `DXSound::UnMuteAll()` |
| `0x1000d850` | 151 | `DXSound::SetVolume(int)` |
| `0x1000d890` | 86 | `DXSound::GetVolume()` |
| `0x1000d8a0` | 9 | `DXSound::CDPlay(int, int)` |
| `0x1000dae0` | 13 | `DXSound::CDStop()` |
| `0x1000dc10` | 10 | `DXSound::CDReplay()` |
| `0x1000dc30` | 12 | `DXSound::CDSetVolume(int)` |
| `0x1000dcc0` | 7 | `DXSound::CDGetVolume()` |
| `0x1000dcd0` | 8 | `DXSound::CDPause()` |
| `0x1000dd10` | 11 | `DXSound::CDResume()` |
| `0x1000dd50` | 128 | `DXSound::PlayStream(char*, int)` |
| `0x1000de70` | 155 | `DXSound::StopStream(int)` |
| `0x1000ded0` | 135 | `DXSound::Refresh()` |
| `0x1000dfd0` | -- | `CreateSoundBufferFromWaveImage(int*, int, int)` |
| `0x1000e1a0` | -- | `RefillStreamingBufferHalf(int)` |
| `0x1000e320` | -- | `WriteToSoundBuffer(int*, void*, int, int)` |
| `0x10013000` | -- | `InitializeCDAudioBackendFromInstallSource(int, int, int)` |

### TD5_d3d.exe Functions

| Address | Name |
|---------|------|
| `0x00414640` | `LoadFrontendSoundEffects` |
| `0x0041ea90` | `ScreenSoundOptions` |
| `0x0042ed50` | `UpdateVehicleEngineSpeedSmoothed` |
| `0x0043bf70` | `RunAudioOptionsOverlay` |
| `0x0043f420` | `UpdateFrontWheelSoundEffects` (visual only, not audio) |
| `0x0043f600` | `UpdateRearWheelSoundEffects` (visual only, not audio) |
| `0x00440a30` | `UpdateVehicleLoopingAudioState` |
| `0x00440b00` | `UpdateVehicleAudioMix` (main mixer, ~800 lines) |
| `0x00441a80` | `LoadVehicleSoundBank` |
| `0x00441c60` | `LoadRaceAmbientSoundBuffers` |
| `0x00441d50` | `ReleaseRaceSoundChannels` |
| `0x00441d90` | `PlayVehicleSoundAtPosition` |
