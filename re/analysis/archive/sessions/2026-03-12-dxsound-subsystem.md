# DXSound Subsystem — M2DX.dll

**Date:** 2026-03-12
**Binary:** M2DX.dll (Ghidra port 8194)
**Scope:** Full map of the DXSound audio subsystem: exports, globals, buffer management, CD-audio backend vtable, streaming, and NoMovie/FixMovie resolution.

---

## 1. DXSound Exports (28 total)

| Address      | Ordinal | Mangled Name | Short Name | Signature |
|---|---|---|---|---|
| `0x1000abf0` | 113 | `?CanDo3D@DXSound@@SAHXZ` | CanDo3D | `int __cdecl ()` — stub, always returns 0. Shares address with `DXPlay::Lobby` (ord 15). |
| `0x1000cda0` | 53  | `?Environment@DXSound@@SAHXZ` | Environment | `int __cdecl ()` — zeros slot table + builds volume attenuation table |
| `0x1000ce30` | 31  | `?Create@DXSound@@SAHXZ` | Create | `int __cdecl ()` — DirectSoundCreate, SetCooperativeLevel, primary buffer, CD backend init |
| `0x1000cfd0` | 42  | `?Destroy@DXSound@@SAHXZ` | Destroy | `int __cdecl ()` — stops stream, releases all 44 base slots, primary, DirectSound |
| `0x1000d070` | 74  | `?GetDSObject@DXSound@@SAHPAPAX0@Z` | GetDSObject | `int __cdecl (void**, void**)` — returns g_pDirectSound + g_pPrimarySoundBuffer |
| `0x1000d090` | 149 | `?SetPlayback@DXSound@@SAHH@Z` | SetPlayback | `int __cdecl (int)` — sets g_isSoundPanEnabled |
| `0x1000d0a0` | 105 | `?Load@DXSound@@SAHPADHH@Z` | Load (3-arg) | `int __cdecl (char*, int slot, int loop)` — delegates to Load(4-arg) with duplicates=1 |
| `0x1000d0c0` | 106 | `?Load@DXSound@@SAHPADHHH@Z` | Load (4-arg) | `int __cdecl (char*, int slot, int loop, int nDuplicates)` — loads WAV file into slot + clones |
| `0x1000d240` | 107 | `?LoadBuffer@DXSound@@SAHPAXHH@Z` | LoadBuffer (3-arg) | `int __cdecl (void*, int slot, int loop)` — delegates to LoadBuffer(4-arg) with duplicates=1 |
| `0x1000d260` | 108 | `?LoadBuffer@DXSound@@SAHPAXHHH@Z` | LoadBuffer (4-arg) | `int __cdecl (void*, int slot, int loop, int nDuplicates)` — in-memory WAV load + clones |
| `0x1000d370` | 109 | `?LoadComplete@DXSound@@SAHXZ` | LoadComplete | `int __cdecl ()` — no-op stub, returns 1. Shares addr with `DXInput::ReadClose` (ord 132). |
| `0x1000d380` | 124 | `?Play@DXSound@@SAHH@Z` | Play (1-arg) | `int __cdecl (int slot)` — plays slot, rotates through duplicate clones if base is busy |
| `0x1000d470` | 125 | `?Play@DXSound@@SAHHHHH@Z` | Play (4-arg) | `int __cdecl (int slot, int vol, int freq, int pan)` — plays slot with explicit overrides |
| `0x1000d560` | 118 | `?ModifyOveride@DXSound@@SAHHHHH@Z` | ModifyOveride | `int __cdecl (int handle, int vol, int freq, int pan)` — ignores mute gate |
| `0x1000d5f0` | 117 | `?Modify@DXSound@@SAHHHHH@Z` | Modify | `int __cdecl (int handle, int vol, int freq, int pan)` — respects mute gate |
| `0x1000d690` | 136 | `?Remove@DXSound@@SAHH@Z` | Remove | `int __cdecl (int slot)` — releases base + all duplicate chain buffers |
| `0x1000d730` | 153 | `?Stop@DXSound@@SAHH@Z` | Stop | `int __cdecl (int handle)` — handle is 1-indexed (subtracts 1 for slot) |
| `0x1000d770` | 152 | `?Status@DXSound@@SAHH@Z` | Status | `int __cdecl (int handle)` — queries if slot is playing (1-indexed handle) |
| `0x1000d7c0` | 119 | `?MuteAll@DXSound@@SAHXZ` | MuteAll | `int __cdecl ()` — sets all buffers to -10000 dB, sets mute flag |
| `0x1000d800` | 159 | `?UnMuteAll@DXSound@@SAHXZ` | UnMuteAll | `int __cdecl ()` — restores per-slot volume presets, clears mute flag |
| `0x1000d850` | 151 | `?SetVolume@DXSound@@SAHH@Z` | SetVolume | `int __cdecl (int vol)` — sets primary buffer attenuation via volume table |
| `0x1000d890` | 86  | `?GetVolume@DXSound@@SAHXZ` | GetVolume | `int __cdecl ()` — returns g_masterSoundVolume |
| `0x1000d8a0` | 9   | `?CDPlay@DXSound@@SAHHH@Z` | CDPlay | `int __cdecl (int track, int loop)` — backend vtable or MCI fallback |
| `0x1000dae0` | 13  | `?CDStop@DXSound@@SAHXZ` | CDStop | `int __cdecl ()` — backend vtable or MCI stop+close |
| `0x1000dc10` | 10  | `?CDReplay@DXSound@@SAHXZ` | CDReplay | `int __cdecl ()` — replays cached track if replay mode active |
| `0x1000dc30` | 12  | `?CDSetVolume@DXSound@@SAHH@Z` | CDSetVolume | `int __cdecl (int vol)` — backend vtable or auxSetVolume fallback |
| `0x1000dcc0` | 7   | `?CDGetVolume@DXSound@@SAHXZ` | CDGetVolume | `int __cdecl ()` — returns g_cdVolume |
| `0x1000dcd0` | 8   | `?CDPause@DXSound@@SAHXZ` | CDPause | `int __cdecl ()` — backend vtable +0x1c or MCI pause |
| `0x1000dd10` | 11  | `?CDResume@DXSound@@SAHXZ` | CDResume | `int __cdecl ()` — backend vtable +0x20 or MCI replay |
| `0x1000dd50` | 128 | `?PlayStream@DXSound@@SAHPADH@Z` | PlayStream | `int __cdecl (char* file, int slot)` — opens WAV, double-buffered streaming |
| `0x1000de70` | 155 | `?StopStream@DXSound@@SAHH@Z` | StopStream | `int __cdecl (int slot)` — stops stream, frees staging buffer |
| `0x1000ded0` | 135 | `?Refresh@DXSound@@SAHXZ` | Refresh | `int __cdecl ()` — streaming tick: cursor poll, half-buffer refill, EOF drain |

**Internal helpers (not exported):**

| Address      | Name | Purpose |
|---|---|---|
| `0x1000dfd0` | CreateSoundBufferFromWaveImage | Parses RIFF/WAVE, fills per-slot WAVEFORMATEX, creates IDirectSoundBuffer |
| `0x1000e1a0` | RefillStreamingBufferHalf | Reads half-buffer from file, pads silence at EOF, uploads via WriteToSoundBuffer |
| `0x1000e320` | WriteToSoundBuffer | Lock/memcpy/Unlock on an IDirectSoundBuffer region |

---

## 2. Sound Buffer / Channel Management

### 2.1 Memory Layout

The DXSound data region spans `0x1005f8f0`–`0x10060cb4`. `Environment()` zeroes 0x46F dwords (4540 bytes) from `0x1005f8f0` to `0x10060aac`, then builds the attenuation table.

```
0x1005f8f0  g_pDirectSound              (IDirectSound*, 4 bytes)
0x1005f8f4  g_soundDuplicateCountTable  (int[44], 176 bytes)
                 indexed as [slot*4 + 0x1005f8f4]
0x1005f9a4  g_soundSlotTable            (SOUND_SLOT[88], stride 0x30, 4224 bytes)
                 slots 0–43: base sample slots
                 slots 44–87: duplicate/clone slots
0x10060a24  --- end of slot table ---
```

### 2.2 Per-Slot Structure (SOUND_SLOT, 0x30 = 48 bytes)

Indexed from base `0x1005f9a4 + slot_index * 0x30`:

| Offset | Size | Field | Notes |
|---|---|---|---|
| +0x00 | 4 | `pBuffer` | IDirectSoundBuffer* (NULL = empty slot) |
| +0x04 | 2 | `wFormatTag` | Always 1 (PCM) |
| +0x06 | 2 | `nChannels` | |
| +0x08 | 4 | `nSamplesPerSec` | |
| +0x0C | 4 | `nAvgBytesPerSec` | |
| +0x10 | 2 | `nBlockAlign` | |
| +0x12 | 2 | `wBitsPerSample` | |
| +0x14 | 2 | `cbSize` | Always 0 |
| +0x16 | 2 | _(padding)_ | |
| +0x18 | 4 | `volumePresetIndex` | Index into attenuation table; restored by UnMuteAll |
| +0x1C | 4 | `frequency` | Last-set playback rate |
| +0x20 | 4 | `pan` | Last-set pan value |
| +0x24 | 4 | `nextDuplicateSlot` | Linked-list index to next clone slot (0 = end) |
| +0x28 | 4 | `modifiedFlag` | Set to 1 when Play(4-arg) applies overrides |
| +0x2C | 4 | `loopFlag` | 0 = one-shot, nonzero = DSBPLAY_LOOPING |

### 2.3 Slot Management Design

- **44 base slots** (indices 0–43): loaded via `Load`/`LoadBuffer`. Index checked: `if (0x2b < param_2) → error`.
- **44 duplicate slots** (indices 44–87): allocated by `DuplicateSoundBuffer` (IDirectSound vtable +0x14) from a base slot. Linked via `nextDuplicateSlot` chain. Counter `g_soundDuplicateCountTable[base_slot]` tracks how many clones exist.
- **Play(1-arg)** rotates through the duplicate chain when the base buffer is already playing, enabling overlapping playback of the same sample.
- **Remove** walks the duplicate chain, releases each clone, then releases the base buffer.
- **Destroy** calls Remove on all 44 base slots.
- **Stop/Status** use 1-indexed handles (subtract 1 for slot index).
- **Modify/ModifyOveride** also use 1-indexed handles. Modify respects mute; ModifyOveride ignores it.

### 2.4 IDirectSoundBuffer Vtable Offsets Used

| Offset | Method | Used In |
|---|---|---|
| +0x08 | Release | Remove, Create cleanup |
| +0x0C | *(IDirectSound)* CreateSoundBuffer | Create (primary), CreateSoundBufferFromWaveImage |
| +0x10 | GetCurrentPosition | Refresh |
| +0x14 | *(IDirectSound)* DuplicateSoundBuffer | Load, LoadBuffer |
| +0x18 | *(IDirectSound)* SetCooperativeLevel | Create |
| +0x24 | GetStatus | Play(1-arg), Status |
| +0x2C | Lock | WriteToSoundBuffer |
| +0x30 | Play | Play (both), PlayStream |
| +0x34 | SetCurrentPosition | Play, PlayStream |
| +0x3C | SetVolume | SetVolume, MuteAll, UnMuteAll, Play(4-arg), Modify |
| +0x40 | SetPan | Play(4-arg), Modify, ModifyOveride |
| +0x44 | SetFrequency | Play(4-arg), Modify, ModifyOveride |
| +0x48 | Stop | Stop, StopStream, Destroy, Play(1-arg clone rotation) |
| +0x4C | Unlock | WriteToSoundBuffer |
| +0x50 | Restore | WriteToSoundBuffer (on Lock failure) |

---

## 3. Scalar DXSound Globals

| Address | Type | Name | Evidence |
|---|---|---|---|
| `0x1005f8f0` | IDirectSound* | g_pDirectSound | Create: `DirectSoundCreate(0, &0x1005f8f0, 0)` |
| `0x1005f8f4` | int[44] | g_soundDuplicateCountTable | Remove asm: `[EAX*4 + 0x1005f8f4]` |
| `0x1005f9a4` | SOUND_SLOT[88] | g_soundSlotTable | MuteAll asm: `MOV ESI, 0x1005f9a4`; Remove asm: `[EBP + 0x1005f9a4]` |
| `0x10060a28` | int | g_isSoundPanEnabled | Environment asm: `MOV [0x10060a28], 1` at end |
| `0x10060a2c` | int | g_streamSoundSlot | PlayStream asm: `MOV [0x10060a2c], ESI` |
| `0x10060a30` | int | g_streamFileHandle | PlayStream asm: `MOV [0x10060a30], EAX` (FOpen result) |
| `0x10060a34` | void* | g_pStreamFileBuffer | PlayStream asm: `MOV [0x10060a34], EAX` (Allocate result) |
| `0x10060a38` | int | g_streamBufferHalfState | PlayStream: set to 1; Refresh: toggles 1↔2 |
| `0x10060a3c` | int | g_streamReadCompletionState | PlayStream: set to 0; RefillStreamingBufferHalf: 0→1→2 |
| `0x10060a40` | int | g_streamStopCursor | RefillStreamingBufferHalf: set at EOF |
| `0x10060a44` | int | g_streamHalfBufferBytes | PlayStream: `MOV [0x10060a44], 0x40b00` (264,960 bytes) |
| `0x10060a48` | DWORD | g_streamPlayCursor | Refresh: from GetCurrentPosition arg 1 |
| `0x10060a4c` | DWORD | g_streamWriteCursor | Refresh: from GetCurrentPosition arg 2 |
| `0x10060a50` | int | g_cdReplayTrack | CDPlay: `-(param_2!=0) & param_1`; CDReplay reads it |
| `0x10060a54` | UINT | g_cdDeviceId | CDPlay: stored from MCI_OPEN wDeviceID |
| `0x10060a58` | UINT_PTR | g_cdAuxDeviceId | CDPlay: set when `wTechnology == 1` (AUXCAPS_CDAUDIO) |
| `0x10060a5c` | struct | g_mciOpenParms | CDPlay: MCI_OPEN_PARMS for `mciSendCommandA(0, MCI_OPEN, ...)` |
| `0x10060a70` | struct | g_mciSetParms | CDPlay: MCI_SET_PARMS (timeformat=10=TMSF) |
| `0x10060a7c` | struct | g_mciPlayParms | CDPlay: MCI_PLAY_PARMS (from/to track fields) |
| `0x10060a88` | DWORD | g_cdVolumeRestore | CDPlay: `= g_cdVolume` at first open; CDStop restores it |
| `0x10060a90` | int | g_cdLastTrackNumber | CDPlay: from MCI_STATUS (number of tracks) |
| `0x10060a94` | IDirectSoundBuffer* | g_pPrimarySoundBuffer | Create asm: `PUSH 0x10060a94` to CreateSoundBuffer |
| `0x10060a98` | int | g_isStreamPlaying | Create: =0; PlayStream: =1; StopStream/Destroy: =0 |
| `0x10060a9c` | int | g_isSoundInitialized | Create: =1; Destroy: =0; checked first in Create |
| `0x10060aa0` | int | g_isCdAudioOpen | CDPlay: =1 after first MCI open; CDStop: =0 |
| `0x10060aa4` | int | g_isSoundMuted | MuteAll: =1; UnMuteAll: =0 |
| `0x10060aa8` | void* | g_pCDAudioBackend | Create: promoted from InitCandidate; all CD methods check it |
| `0x10060aac` | int | g_masterSoundVolume | SetVolume asm: `MOV [0x10060aac], ESI`; GetVolume returns it |
| `0x10060ab0` | int[128] | g_soundVolumeAttenuationTable | Environment: log-scale table, 128 entries to `0x10060cb0` |
| `0x10060cb0` | int | g_cdVolume | CDSetVolume: `MOV [0x10060cb0], param_1`; CDGetVolume returns it |
| `0x10060cb4` | int | g_cdPlaybackLoopParam | CDPlay: stores param_2 (loop/replay flag) |

---

## 4. CD-Audio Backend (wv2wav)

### 4.1 Initialization Path

`DXSound::Create` → `InitializeCDAudioBackendFromInstallSource(app+4, pDirectSound, pPrimaryBuffer)`:

1. `CreateArchiveProviderManager(app+4, 0, 0)` → `g_pArchiveProviderManager` (`0x10061c80`)
2. Manager vtable +0x14 with class ID `0x6000` → `g_pCDAudioBackendInitCandidate` (`0x10061c84`)
3. Manager vtable +0x14 with class ID `0x1005` → `g_pRegistryConfigProvider` (`0x10061c88`)
4. Backend vtable +0x0C: `BindDirectSound(0, pDirectSound, pPrimaryBuffer)`
5. Registry provider vtable +0x34: reads `HKLM\Software\Microsoft\Windows\CurrentVersion\InstallSource`
6. Appends `\` if missing, appends `TD5_Track_.Wv2_.`
7. Backend vtable +0x10: `OpenArchive(path, "wv2wav", 2, 1)`
8. On success: `g_pCDAudioBackend = g_pCDAudioBackendInitCandidate`
9. On failure: releases all three objects, g_pCDAudioBackend stays NULL → MCI fallback

### 4.2 CD-Audio Backend Vtable

All DXSound CD methods check `g_pCDAudioBackend != NULL` first. If present, they dispatch through the vtable. Otherwise they fall back to MCI cdaudio + auxSetVolume.

| Vtable Offset | Semantics | Called From | Signature |
|---|---|---|---|
| +0x08 | Release | InitializeCDAudioBackendFromInstallSource (cleanup) | `void ()` |
| +0x0C | BindDirectSound | InitializeCDAudioBackendFromInstallSource | `bool (int zero, IDirectSound*, IDirectSoundBuffer*)` |
| +0x10 | OpenArchive | InitializeCDAudioBackendFromInstallSource | `bool (char* path, char* codec, int, int)` |
| +0x14 | PlayTrack | CDPlay | `int (int zero, int trackNum, bool loop)` → returns bool in low byte |
| +0x18 | StopPlayback | CDStop | `int ()` → returns bool in low byte |
| +0x1C | PausePlayback | CDPause | `int ()` → returns bool in low byte |
| +0x20 | ResumePlayback | CDResume | `int ()` → returns bool in low byte |
| +0x30 | SetVolume | CDSetVolume, CDStop (restore) | `void (float normalizedVolume)` — `(byte >> 8) * (1/252)` |

The backend implements a "wv2wav" codec — likely decompressing pre-ripped CD tracks from archive files on the install media, playing them through DirectSound rather than requiring the physical CD.

### 4.3 MCI Fallback Path

When `g_pCDAudioBackend == NULL`, CD methods use Win32 MCI:

- **CDPlay**: `MCI_OPEN` with device type "cdaudio", `MCI_SET` timeformat TMSF, `MCI_PLAY` with from/to track. Enumerates `auxGetDevCaps` looking for `AUXCAPS_CDAUDIO` (wTechnology==1) for volume control.
- **CDStop**: `MCI_STOP`, restores volume via `auxSetVolume`, `MCI_CLOSE`.
- **CDPause**: `MCI_PAUSE`.
- **CDResume**: Calls `CDPlay(g_cdReplayTrack, 1)` (re-plays from start — no true MCI resume).
- **CDSetVolume**: `auxSetVolume(g_cdAuxDeviceId, vol)`.
- **CDReplay**: If `g_cdReplayTrack > 0`, calls `CDPlay(track, 1)`.

---

## 5. Streaming Subsystem

PlayStream implements double-buffered WAV streaming:

1. **Open**: `FOpen(file, 0)` → `g_streamFileHandle`
2. **Allocate**: `0x81600` bytes (2 × `0x40b00` = 529,920 bytes) → `g_pStreamFileBuffer`
3. **Preload**: `FRead` fills both halves
4. **Create buffer**: `CreateSoundBufferFromWaveImage` with streaming flag, then `SetCurrentPosition(0)` + `Play(0, 0, DSBPLAY_LOOPING)`
5. **Tick** (`Refresh`): polls play cursor via `GetCurrentPosition`. When cursor crosses half boundary, calls `RefillStreamingBufferHalf` for the consumed half.
6. **EOF**: `RefillStreamingBufferHalf` pads remaining bytes with silence (0x80 for 8-bit, 0x00 for 16-bit). Sets `g_streamReadCompletionState` 0→1 (first half done) →2 (second half done).
7. **Drain**: When state==2 and play cursor passes `g_streamStopCursor`, stops and releases.

Constants: half-buffer = 264,960 bytes (`0x40b00`), total staging = 529,920 bytes.

---

## 6. Volume System

`Environment()` precomputes `g_soundVolumeAttenuationTable[128]` using a logarithmic formula:

```
for i in 0..127:
    attenuation[i] = -round(log_scale(i))  // negative centibel values
```

The table maps logical volume indices (0–127) to DirectSound attenuation values (negative centibels). `SetVolume(vol)` uses `vol >> 9` as the table index (so logical volume 0–65535 maps to 128 table entries).

`MuteAll` sets all active buffers to `0xFFFFD8F0` = −10000 (DSBVOLUME_MIN). `UnMuteAll` restores each buffer's cached `volumePresetIndex` from the table.

---

## 7. NoMovie / FixMovie Resolution

**Confirmed: NOT connected to the DXSound subsystem.**

Both are startup option tokens parsed by `ApplyStartupOptionToken` (`0x10012860`):

| Token | Table Index | Target Address | Value Written |
|---|---|---|---|
| `NoMovie` | 14 (0x0E) | `0x10061c0c` | 4 |
| `FixMovie` | 16 (0x10) | `0x10061c44` | 1 |

- **Write xrefs**: 1 each, from `ApplyStartupOptionToken` only
- **Read xrefs**: 0 in M2DX.dll — no consumer found
- These are dead/exported state flags. Likely consumed by the EXE or by M2DXFX.dll (movie/FMV subsystem) via direct memory access or `GetProcAddress` on the DLL's data segment. They control FMV movie playback behavior, not audio.

The full token table at `0x10027bf8` (26 entries) includes:

| Index | Token | Effect |
|---|---|---|
| 0–4 | `//`, `rem`, `;`, `\`, `#` | Comment delimiters (return 0, no-op) |
| 5 | `Log` | `bLogPrefered = 1` |
| 6 | `TimeDemo` | `DAT_10061c40 = 1` |
| 7 | `FullScreen` | `DAT_10061c1c = 1` |
| 8 | `Window` | `DAT_10061c1c = 0` |
| 9–10 | `DoubleBuffer`, `NoTripleBuffer` | `g_tripleBufferState = 4` |
| 11 | `NoWBuffer` | `g_worldTransformState = 4` |
| 12 | `Primary` | `g_adapterSelectionOverride = 1` |
| 13 | `DeviceSelect` | `g_adapterSelectionOverride = -1` |
| 14 | **`NoMovie`** | `DAT_10061c0c = 4` |
| 15 | `FixTransparent` | `DAT_100294b4 = 1` |
| 16 | **`FixMovie`** | `DAT_10061c44 = 1` |
| 17 | (unmapped) | `DAT_1003266c = 4` |
| 18 | (unmapped) | `g_skipPrimaryDriverTestState = 1` |
| 19 | (unmapped) | `g_ddrawEnumerateExState = 4` |
| 20 | (unmapped) | `g_mipFilterState = 4` |
| 21 | (unmapped) | `g_lodBiasState = 4` |
| 22 | (unmapped) | `g_zBiasState = 4` |
| 23 | `MatchBitDepth` | `g_matchBitDepthState = 1` |
| 24 | `WeakForceFeedback` | `FFGainScale = 0.5` |
| 25 | `StrongForceFeedback` | `FFGainScale = 1.0` |

---

## 8. Proposed Renames

| Address | Binary | Current Name | Proposed Name | Confidence | Evidence |
|---|---|---|---|---|---|
| `0x1005f8f0` | M2DX.dll | DAT_1005f8f0 / g_pDirectSound | g_pDirectSound | confirmed | DirectSoundCreate target; SetCooperativeLevel/CreateSoundBuffer/DuplicateSoundBuffer caller |
| `0x1005f8f4` | M2DX.dll | DAT_1005f8f4 | g_soundDuplicateCountTable | confirmed | Remove: `[EAX*4+0x1005f8f4]` used as duplicate chain length counter |
| `0x1005f9a4` | M2DX.dll | DAT_1005f9a4 | g_soundSlotTable | confirmed | MuteAll base; Remove/Play/Load all index here; 88 slots × 0x30 stride |
| `0x10060a28` | M2DX.dll | DAT_10060a28 | g_isSoundPanEnabled | confirmed | Environment sets to 1; SetPlayback writes param; Play/Modify check it |
| `0x10060a2c` | M2DX.dll | DAT_10060a2c | g_streamSoundSlot | confirmed | PlayStream stores param_2; Refresh/StopStream/Destroy read it |
| `0x10060a30` | M2DX.dll | DAT_10060a30 | g_streamFileHandle | confirmed | PlayStream: `FOpen` result stored here; RefillStreamingBufferHalf: `FRead`/`FClose` arg |
| `0x10060a34` | M2DX.dll | DAT_10060a34 | g_pStreamFileBuffer | confirmed | PlayStream: `Allocate(0x81600)` result; StopStream/Destroy: `DeAllocate` arg |
| `0x10060a38` | M2DX.dll | DAT_10060a38 | g_streamBufferHalfState | confirmed | PlayStream: =1; Refresh: toggles 1↔2 for ping-pong |
| `0x10060a3c` | M2DX.dll | DAT_10060a3c | g_streamReadCompletionState | confirmed | PlayStream: =0; RefillStreamingBufferHalf: 0→1→2 at EOF |
| `0x10060a40` | M2DX.dll | DAT_10060a40 | g_streamStopCursor | confirmed | RefillStreamingBufferHalf sets; Refresh compares to play cursor for drain |
| `0x10060a44` | M2DX.dll | DAT_10060a44 | g_streamHalfBufferBytes | confirmed | PlayStream: =0x40b00; used as FRead size and Lock length |
| `0x10060a48` | M2DX.dll | DAT_10060a48 | g_streamPlayCursor | confirmed | Refresh: `GetCurrentPosition(&0x10060a48, ...)` arg 1 |
| `0x10060a4c` | M2DX.dll | DAT_10060a4c | g_streamWriteCursor | strongly-suspected | Refresh: `GetCurrentPosition(..., &0x10060a4c)` arg 2 |
| `0x10060a50` | M2DX.dll | DAT_10060a50 | g_cdReplayTrack | confirmed | CDPlay: conditional store; CDReplay: `if (g_cdReplayTrack > 0) CDPlay(it, 1)` |
| `0x10060a54` | M2DX.dll | DAT_10060a54 | g_cdDeviceId | confirmed | CDPlay: stored from MCI_OPEN wDeviceID result; used in all MCI calls |
| `0x10060a58` | M2DX.dll | DAT_10060a58 | g_cdAuxDeviceId | confirmed | CDPlay: set when auxGetDevCaps.wTechnology==1; CDSetVolume: auxSetVolume arg |
| `0x10060a5c` | M2DX.dll | DAT_10060a5c | g_mciOpenParms | strongly-suspected | CDPlay: address passed to mciSendCommandA for MCI_OPEN |
| `0x10060a70` | M2DX.dll | DAT_10060a70 | g_mciSetParms | strongly-suspected | CDPlay: address passed to mciSendCommandA for MCI_SET |
| `0x10060a7c` | M2DX.dll | DAT_10060a7c | g_mciPlayParms | strongly-suspected | CDPlay: address passed to mciSendCommandA for MCI_PLAY |
| `0x10060a88` | M2DX.dll | DAT_10060a88 | g_cdVolumeRestore | strongly-suspected | CDPlay: `= g_cdVolume` at first open; CDStop: restored via auxSetVolume |
| `0x10060a90` | M2DX.dll | DAT_10060a90 | g_cdLastTrackNumber | confirmed | CDPlay: from MCI_STATUS track count; compared to select MCI_PLAY flags |
| `0x10060a94` | M2DX.dll | DAT_10060a94 / g_pPrimarySoundBuffer | g_pPrimarySoundBuffer | confirmed | Create: CreateSoundBuffer into this addr; SetVolume: vtable+0x3c target |
| `0x10060a98` | M2DX.dll | DAT_10060a98 / g_isStreamPlaying | g_isStreamPlaying | confirmed | Create: =0; PlayStream: =1; StopStream: =0; Refresh checks it |
| `0x10060a9c` | M2DX.dll | DAT_10060a9c / g_isSoundInitialized | g_isSoundInitialized | confirmed | Create: =1; Destroy: =0; checked at top of Create and Load |
| `0x10060aa0` | M2DX.dll | DAT_10060aa0 | g_isCdAudioOpen | confirmed | CDPlay: =1 after MCI open; CDStop: =0 after MCI close |
| `0x10060aa4` | M2DX.dll | DAT_10060aa4 | g_isSoundMuted | confirmed | MuteAll: =1; UnMuteAll: =0; Play/Modify check it |
| `0x10060aa8` | M2DX.dll | DAT_10060aa8 / g_pCDAudioBackend | g_pCDAudioBackend | confirmed | Create: promoted from InitCandidate; all CD methods dispatch through it |
| `0x10060aac` | M2DX.dll | DAT_10060aac | g_masterSoundVolume | confirmed | SetVolume stores; GetVolume returns; Environment zeroes region |
| `0x10060ab0` | M2DX.dll | DAT_10060ab0 / g_soundVolumeAttenuationTable | g_soundVolumeAttenuationTable | confirmed | Environment builds 128-entry log table; SetVolume/Modify/UnMuteAll index into it |
| `0x10060cb0` | M2DX.dll | DAT_10060cb0 | g_cdVolume | confirmed | CDSetVolume stores; CDGetVolume returns; CDPlay reads for initial restore |
| `0x10060cb4` | M2DX.dll | DAT_10060cb4 | g_cdPlaybackLoopParam | strongly-suspected | CDPlay: stores param_2 (loop boolean) |
| `0x1000dfd0` | M2DX.dll | CreateSoundBufferFromWaveImage | CreateSoundBufferFromWaveImage | confirmed | Parses RIFF "WAVE" header, extracts "fmt " + "data" chunks |
| `0x1000e1a0` | M2DX.dll | RefillStreamingBufferHalf | RefillStreamingBufferHalf | confirmed | Called from Refresh with half=1 or half=2; reads file + uploads |
| `0x1000e320` | M2DX.dll | WriteToSoundBuffer | WriteToSoundBuffer | confirmed | Lock/memcpy/Unlock pattern with Restore fallback |
| `0x10061c0c` | M2DX.dll | DAT_10061c0c | g_noMovieFlag | strongly-suspected | "NoMovie" token writes 4 here; no read xrefs in M2DX.dll; NOT audio |
| `0x10061c44` | M2DX.dll | DAT_10061c44 | g_fixMovieFlag | strongly-suspected | "FixMovie" token writes 1 here; no read xrefs in M2DX.dll; NOT audio |

---

## 9. Architecture Summary

```
                    DXSound::Create
                         │
              ┌──────────┴──────────┐
              │                     │
    DirectSoundCreate       InitializeCDAudioBackendFromInstallSource
              │                     │
     g_pDirectSound         ┌───────┴────────┐
              │              │                │
    CreateSoundBuffer    CreateArchive    g_pCDAudioBackend
              │          ProviderManager      │
     g_pPrimarySoundBuffer     │         wv2wav vtable
                          classId 0x6000      │
                               │         PlayTrack / StopPlayback
                          classId 0x1005  PausePlayback / ResumePlayback
                               │         SetVolume
                          Registry read
                          (InstallSource)

    ┌─────────────────────────────────────────────────────────────┐
    │                    Sound Slot Table                         │
    │  g_soundSlotTable[0..43]  = base sample slots              │
    │  g_soundSlotTable[44..87] = duplicate clone slots           │
    │  each: IDirectSoundBuffer* + WAVEFORMATEX + vol/freq/pan   │
    │  linked-list chains for overlapping playback                │
    └─────────────────────────────────────────────────────────────┘

    ┌─────────────────────────────────────────────────────────────┐
    │                    Streaming Engine                         │
    │  PlayStream → FOpen → Allocate(2×half) → preload           │
    │  Refresh (tick) → GetCurrentPosition → RefillHalf           │
    │  Double-buffered, 264KB half-buffers, silence-padded EOF   │
    └─────────────────────────────────────────────────────────────┘

    ┌─────────────────────────────────────────────────────────────┐
    │                CD Audio (dual-path)                         │
    │  if g_pCDAudioBackend: wv2wav vtable dispatch              │
    │  else: MCI cdaudio + auxSetVolume fallback                 │
    └─────────────────────────────────────────────────────────────┘
```

---

## 10. Open Questions

1. **wv2wav codec implementation**: The CD audio backend is instantiated via `CreateArchiveProviderManager` with class ID `0x6000`. The actual codec lives in the provider DLL (likely on the install CD). Its internal structure is opaque from M2DX.dll alone.
2. **Backend vtable slots +0x24, +0x28, +0x2C**: Not called by any DXSound method. May be query methods (GetPosition, GetTrackCount, IsPlaying).
3. **g_noMovieFlag / g_fixMovieFlag consumers**: Write-only in M2DX.dll. Likely read by TD5_d3d.exe or M2DXFX.dll — needs cross-binary xref search.
4. **Startup token indices 17–22**: String addresses known but not yet decoded. Likely display/rendering options (one is near "NoWBuffer", "Primary" etc.).
5. **g_freeDuplicateBufferSlotCount**: Referenced in Load/LoadBuffer decompilation but exact address not pinpointed from disassembly. Lives within the zeroed region; decremented as clones are allocated.
