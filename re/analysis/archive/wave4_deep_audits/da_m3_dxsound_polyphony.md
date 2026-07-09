# DA-M3 — M2DX DXSound Polyphony Deep Audit

Date: 2026-05-22
Scope: M2DX.dll — DXSound::Create / Play / Load / LoadBuffer / SetVolume /
CDSetVolume; g_soundDuplicateCountTable; InitializeCDAudioBackendFromInstallSource.
Goal: document complete polyphony architecture so port td5_sound.c can be
diffed and the engine/skid choppiness root-caused.

Pool slot: TD5_pool6 (M2DX.dll, read-only). TD5_d3d.exe consulted via TD5_pool12.

---

## Section A — Original sound bank architecture (slots, channels, duplicates)

### A.1 Per-slot record layout (stride 0x30 bytes = 48 B)

Table at `g_soundBufferTable @ 0x1005f9a4` — 88 records × 48 bytes (44 head + 44 dup).

| Offset | Symbol                       | Purpose                                          |
| ------ | ---------------------------- | ------------------------------------------------ |
| +0x00  | g_soundBufferTable + N*0x30  | LPDIRECTSOUNDBUFFER                              |
| +0x04..0x16 | PCMWAVEFORMATEX inline (wFormatTag=1, nCh, sRate, avgB/s, blkAlign, bps, cb) |
| +0x24  | DAT_1005f9c8 + N*0x30        | next-duplicate slot index (linked-list)          |
| +0x2c  | DAT_1005f9d0 + N*0x30        | loopFlag (DSBPLAY_LOOPING)                       |

### A.2 Slot ranges (from EXE load callers)

- 1..10  Frontend SFX (Ping1/2/3, Crash1, Whoosh, Uh_Oh) — LoadFrontendSoundEffects
- 0..17  Per-vehicle Drive/Rev/Horn (slot = veh*3 + {0,1,2}) — LoadVehicleSoundBank ×6
- 18..36 Race ambient + weather (Rain, SkidBit, Siren3/5, …) — LoadRaceAmbientSoundBuffers
- 37..43 Traffic-vehicle engine variant loops — LoadRaceAmbientSoundBuffers tail
- 44..87 **Duplicate clone pool** (DuplicateSoundBuffer, alloc'd at Load time)

### A.3 Capacity constants

- Named base slots: 0x2c = 44 (cap: `if (param_2 < 0x2c)` in LoadBuffer)
- Total slots: 0x58 = 88 (44 base + 44 dup)
- `g_freeDuplicateBufferSlotCount` starts at 44; decremented per
  DuplicateSoundBuffer success. Loop guard `< 0x2c`. Exhaustion silently
  degrades nDup>1 → nDup=1.

### A.4 g_soundDuplicateCountTable @ 0x1005f8f4

`int[44]`, one count per base slot, **including the head** (nDup=1→count=1,
nDup=2→count=2 after one DuplicateSoundBuffer success).

---

## Section B — DXSound::Play(slotId) decision tree (0x1000d380)

```
Play(slot):
  iVar3 = slot * 0x30                       ; byte offset into per-slot fields
  piVar4 = g_soundBufferTable[slot]         ; LPDIRECTSOUNDBUFFER for head slot
  if g_pDirectSound == NULL: return 0
  if piVar4 == NULL:        return 0        ; slot not loaded
  if g_isSoundMuted != 0:   return 0

  status = piVar4->GetStatus(piVar4, &out)  ; vtable [0x24]
  if (status_flags == 1):                   ; PLAYING flag set
      if (g_soundDuplicateCountTable[slot] < 2):
          ; No duplicates → reset cursor on head (cursor-snap = the choppiness)
          piVar4->Stop(piVar4)               ; vtable [0x48]
          piVar4->SetCurrentPosition(piVar4, 0)  ; vtable [0x34]
      else:
          ; ≥2 voices → walk linked list looking for an idle dup
          next = slot_table[slot].+0x24      ; first duplicate slot index
          remaining = dupCount - 1
          while (remaining != 0):
              piVar4 = g_soundBufferTable[next]
              next_next = slot_table[next].+0x24
              piVar4->GetStatus(&out)
              if (next_next == 0):
                  ; reached end of chain → steal this voice (cursor-snap it)
                  piVar4->Stop()
                  piVar4->SetCurrentPosition(0)
                  break
              else:
                  remaining--                ; continue walking
                  next = next_next

  ; After voice-selection, start the chosen buffer
  loopFlag = slot_table[slot].+0x2c          ; persisted from Load time
  if (loopFlag == 0):
      piVar4->Play(0, 0, 0)                  ; one-shot
  else:
      piVar4->Play(0, 0, 1)                  ; DSBPLAY_LOOPING

  return (handle = local_4 + 1)              ; encoded slot for Stop()
```

Voice-selection summary:

- Loop walks while `next_next != 0`; at chain-tail uses that slot regardless
  of playing-state — "tail-steal" semantics.
- 2-dup slot → double-buffered voice trading: re-Play while head is running
  routes to the dup so the head plays out cleanly; only the dup cursor-snaps.
- 1-dup slot → every re-Play while already running = Stop+SetCurrentPosition(0) → click/cut.

This is why orig Drive/Rev/Horn never sound choppy under rapid state-machine
retrigger: the second voice provides crossfade headroom. Port has none.

---

## Section C — g_soundDuplicateCountTable schema + per-sound counts

Sourced by tracing every 4-arg Load/LoadBuffer caller in TD5_d3d.exe.
The 3-arg overloads at 0x1000d0a0 / 0x1000d240 simply delegate to the 4-arg
with `nDuplicates=1`.

| Slot range  | Sound name(s)                                                  | Loop | nDup | Caller                          |
| ----------- | -------------------------------------------------------------- | ---- | ---- | ------------------------------- |
| 1           | Ping3.wav                                                      | 0    | 1    | LoadFrontendSoundEffects (0x414640) |
| 2           | Ping2.wav                                                      | 0    | 1    | "                               |
| 3           | Ping1.wav                                                      | 0    | 1    | "                               |
| 4           | Crash1.wav (used 3 times)                                      | 0    | 1    | "                               |
| 5,7         | Whoosh.wav (used 3 times)                                      | 0    | 1    | "                               |
| 9           | Crash1.wav                                                     | 0    | 1    | "                               |
| 10          | Uh_Oh.wav                                                      | 0    | 1    | "                               |
| veh*3+0     | Drive.wav (loop)  veh∈[0..5] → slots 0,3,6,9,12,15             | 1    | **2**| LoadVehicleSoundBank (0x441b4f) |
| veh*3+1     | Rev.wav or Reverb.wav (loop) → slots 1,4,7,10,13,16            | 1    | **2**| LoadVehicleSoundBank (0x441bd1) |
| veh*3+2     | Horn.wav → slots 2,5,8,11,14,17                                | 0    | **2**| LoadVehicleSoundBank (0x441c26) |
| 18+i (i<4)  | Rain.wav, SkidBit.wav, Siren3.wav, Siren5.wav (loop)           | 1    | **2**| LoadRaceAmbientSoundBuffers (0x441cd7) |
| 18+i (i≥4)  | other ambients (one-shot)                                      | 0    | **2**| "                               |
| 37+i        | TrafficEngine variant loops                                    | 1    | **2**| LoadRaceAmbientSoundBuffers (0x441d25) |

Take-aways:

- Frontend SFX nDup=1 (acceptable: one menu click at a time).
- Vehicle Drive/Rev/Horn nDup=2 → 6 vehicles × 3 × 2 = 36 of 44 free dups.
- Ambient slots 18..36 nDup=2 (incl. SkidBit.wav slot 19) — **skid choppiness
  shares root cause with engine choppiness**.
- Traffic variants (37+) nDup=2.
- Full race demand: 18 + 19 + ≤6 = ≤43 of 44 dups (1 spare).

---

## Section D — CDAudio backend init flow (InitializeCDAudioBackendFromInstallSource @ 0x10012eb0)

This is **not the typical mciSendCommand CD-DA path**. It's a fully optional
archive-backed music subsystem:

```
InitializeCDAudioBackendFromInstallSource(hInstance, pDirectSound, pPrimaryBuffer):
    g_pArchiveProviderManager = CreateArchiveProviderManager(hInstance, 0, 0)
    if (!manager) → false

    manager->AcquireProvider(0x6000, &g_pCDAudioBackendInitCandidate)   ; "audio backend"
    if (!candidate) → goto cleanup

    manager->AcquireProvider(0x1005, &g_pRegistryConfigProvider)        ; "registry provider"
    if (!registry)  → goto cleanup

    candidate->vtable[0x0c](0, pDirectSound, pPrimaryBuffer)   ; bind DS/primary
    if FAILED → cleanup

    registry->vtable[0x34](HKLM, "Software\\Microsoft\\Windows\\CurrentVersion",
                           "InstallSource", &localPathBuf)
    if FAILED → cleanup

    ; Append "TD5_Track.Wv2" filename to InstallSource path
    strcat-ish ops with literal "TD5_Track.Wv2"

    candidate->vtable[0x10](fullPath, "wv2wav", 2, 1)          ; "open archive, codec=wv2wav"
    if SUCCESS → return true (g_pCDAudioBackend = candidate)
```

Class IDs:
- `0x6000` — CD audio backend (DirectSound-aware decoder)
- `0x1005` — Registry/config provider

Format: TD5 ships **TD5_Track.Wv2** (a wavelet-packed archive containing music
tracks) on disc. The path is reconstructed from the registry's "InstallSource"
key (set by setup.exe). Codec name `"wv2wav"`. Vtable layout for the backend:
- +0x08  Release
- +0x0c  Initialize(unused, IDirectSound*, IDirectSoundBuffer*)
- +0x10  OpenArchive(path, codecName, ?, ?)
- +0x30  SetVolume(float)            ← used by DXSound::CDSetVolume

DXSound::CDSetVolume @ 0x1000dc30 then dispatches to either:
- `g_pCDAudioBackend->vtable[0x30]((float)((vol>>8)&0xff) * _DAT_1001d280)`
  — the wv2wav archive backend (preferred)
- `auxSetVolume(g_cdAuxDeviceId, raw)` — Win32 wave auxiliary fallback for
  redbook CD-DA on systems without the archive (legacy mode).

This is the music-track integration reference for the port. Currently
`td5_sound.c` does not implement either path — music is silent in the port.

---

## Section E — Port-side divergences → actionable items

Module: `td5mod/src/td5re/td5_sound.c` (1569 lines) +
        `td5mod/src/td5re/td5_platform_win32.c` (DirectSound layer).

### E.1 [PRIMARY ROOT CAUSE OF CHOPPINESS] No duplicate-buffer polyphony

`td5_sound.c:1531`:
```c
if (duplicates > 0 && slot + TD5_SOUND_DUP_OFFSET < TD5_SOUND_TOTAL_SLOTS)
    s_slot_to_buffer[slot + TD5_SOUND_DUP_OFFSET] = buffer_id;
```

The port writes the **same buffer_id** into the duplicate slot. It does not
allocate a second DirectSound buffer. So `slot_play(slot)` and
`slot_play(slot+44)` both end up at `td5_plat_audio_play(buffer_id, …)`
which (in `td5_platform_win32.c:1848`) calls
`IDirectSound8_DuplicateSoundBuffer` at **play time** — a new DS dup buffer
per Play() — but stores the channel only in `s_slot_to_channel[slot]`,
overwriting any previously-tracked channel for the same slot.

Consequence: when the engine state machine in `td5_sound.c:893..913` flips
gear (DRIVE↔REV) at high cycle rate, `slot_play(next_slot, …)` is invoked
while the previous Play() handle still owns a running buffer; the port
calls `slot_stop(cur_state + veh*3 + slot_offset)` first
(line 907) which Releases the running buffer immediately — the click. Orig's
nDup=2 ring lets the previous voice fade naturally while the new one starts
on a duplicate.

Specific symptoms this explains:
1. Drive→Rev gear flip click (engine SFX).
2. Skid start-stop sputter (SkidBit at slot 19 has nDup=2 in orig,
   nDup=mirror-of-1 in port).
3. Sirens 3/5 (slot 20/21) likely also affected during cop-chase.
4. Horn double-tap (slot veh*3+2) cuts itself off.

Fix outline (small surgery, no API rework):

1. In `sound_load_wav_from_zip`, allocate a **second buffer_id** when
   `duplicates >= 2` by calling `td5_plat_audio_load_wav` again with the
   same bytes, store it as the dup_id. (Or call a new
   `td5_plat_audio_duplicate(buffer_id)` that calls
   `IDirectSoundDuplicateSoundBuffer` once at load-time.)
2. Add a small per-slot "next voice idx" rotating counter (2 entries).
3. In `slot_play(slot, …)`, before issuing `td5_plat_audio_play`:
   - check `td5_plat_audio_is_playing(s_slot_to_channel[slot])`;
   - if playing and the slot has a stored dup channel that is NOT playing,
     route the new play to the dup channel instead;
   - if both are playing, **do not** stop+restart; just let the new attempt
     replace the older voice on the dup (matches orig's "tail steal"
     semantics).

### E.2 [SECONDARY] Frontend SFX path uses `td5_plat_audio_play` directly
`td5_sound.c:1370` plays the frontend SFX without going through `slot_play`,
which is fine since orig also gives those nDup=1. No change needed.

### E.3 [DEFERRED — music tracks] CDAudio backend not implemented
`td5_sound.c` has no equivalent to
`InitializeCDAudioBackendFromInstallSource`. The TD5_Track.Wv2 archive is
the canonical music source for orig. Two options for parity:
- Decode TD5_Track.Wv2 (wv2wav codec) and stream via DSound — requires
  reverse-engineering of the wv2 codec; not a polyphony issue.
- Extract music tracks to standard .ogg/.wav at install time and play via
  `td5_plat_audio_stream_play` (already exists at td5_platform_win32.c:1916).
- CDSetVolume currently has no callee.

This is **outside the choppiness class** but documented here per scope point 4.

### E.4 [VERIFY] Engine state-machine off-by-one slot reference
`td5_sound.c:907` stops slot `cur_state + veh*3 + slot_offset` but `cur_state`
takes ENGINE_STATE_DRIVE / ENGINE_STATE_REV values. Verify that this maps
to slot offsets {0,1} so that the stop hits Drive/Rev correctly; the orig
slot layout is veh*3+0=Drive, veh*3+1=Rev, veh*3+2=Horn. If
ENGINE_STATE_DRIVE=1 then `1 + veh*3 = veh*3+1` is Rev, not Drive — possible
off-by-one cross-stop. Worth a runtime log diff.

### E.5 Channel pool size (low priority)
`MAX_AUDIO_CHANNELS = 64` in `td5_platform_win32.c:104`. Orig effectively
needs ~88 (44 head + 44 dup) but reuses slots aggressively via the
linked-list voice-stealing, so concurrent voices rarely exceed ~20. The port
pool of 64 is comfortable for both head-only and head+dup designs. No
change required for fix E.1.

---

## Appendix — Symbol & address index

M2DX.dll: Create 0x1000ce30, Destroy 0x1000cfd0, Load(3) 0x1000d0a0,
Load(4) 0x1000d0c0, LoadBuffer(3) 0x1000d240, LoadBuffer(4) 0x1000d260,
Play 0x1000d380, Remove 0x1000d690, Stop 0x1000d730, SetVolume 0x1000d850,
CDSetVolume 0x1000dc30, CreateSoundBufferFromWaveImage 0x1000dfd0,
InitializeCDAudioBackendFromInstallSource 0x10012eb0,
g_soundDuplicateCountTable 0x1005f8f4 (int[44]),
g_soundBufferTable 0x1005f9a4 (SoundSlot[88], stride 0x30),
g_freeDuplicateBufferSlotCount 0x10060a24.

TD5_d3d.exe: LoadFrontendSoundEffects 0x00414640, LoadVehicleSoundBank 0x00441a80,
LoadRaceAmbientSoundBuffers 0x00441c60.

Port (td5mod/src/td5re):
- td5_sound.c:1492–1540 sound_load_wav_from_zip (dup stub — line 1531 mirror)
- td5_sound.c:127–149 slot_play/stop/modify/is_playing
- td5_sound.c:893–913 engine state retrigger
- td5_sound.c:940–963 skid start/stop
- td5_platform_win32.c:1832–1869 td5_plat_audio_play
- td5_platform_win32.c:104 MAX_AUDIO_CHANNELS=64

