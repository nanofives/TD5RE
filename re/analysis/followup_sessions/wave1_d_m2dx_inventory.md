# Wave 1 Agent D — M2DX.dll Inventory

Scope: enumerate and classify functions in M2DX.dll (orig middleware) for read-only analysis.
Method: Ghidra pool slot `TD5_pool14` (read-only). M2DX.dll already imported.
Image base: `0x10000000`. Date: 2026-05-22.

---

## Section A: M2DX project status

- **Accessible**: yes. Already imported in **every** pool slot (verified via `TD5_pool14`).
- **Location**: `C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_pool\TD5_pool14.rep\M2DX.dll`
- **Sibling DLLs in project**: `Language.dll`, `M2DXFX.dll`, plus `TD5_d3d.exe`.
- **Function count**: **446 total**, of which:
  - **191 named exports** (per `external_exports_list`) organized into 8 namespaces (DXD3D/DXD3DTexture/DXDraw/DXInput/DXPlay/DXSound/DXWin/DX) plus 15 free/global stubs.
  - **147 CRT_\* internals** (MSVC runtime — fopen/fread/malloc/SEH/etc., not part of game API).
  - 19 unwind/SEH helpers, 8 import thunks, ~81 unnamed helpers under the namespaces above.
- **Analysis quality**: classes are already named with C++-style namespaces (`DXSound::CDPlay` etc.) and many functions carry plate-comments. Mangled-name hints (`?Play@DXSound@@SAHH@Z`) preserved at each entry, so ABI parity work is one decomp away.

---

## Section B: Subsystem breakdown

| Namespace        | Exports | Role | Example exports |
|------------------|---------|------|-----------------|
| **DXD3D**        | 16 | Direct3D 3 device + driver bootstrap | `Environment`, `Create`, `Destroy`, `BeginScene`, `EndScene`, `CanFog`, `SetRenderState`, `TextureClamp`, `GetMaxTextures`, `FullScreen`, `ChangeDriver`, `GetStats` |
| **DXD3DTexture** | 14 | Texture upload, palette, masking, lose/restore | `Load`, `LoadRGB`, `LoadRGBS24`, `LoadRGBS32`, `Manage`, `GetMask`, `LoseAll`, `RestoreAll`, `ClearAll` |
| **DXDraw**       | 23 | DirectDraw 7 surfaces, blit, flip, gamma, FPS print | `Flip`, `ClearBuffers`, `GammaControl`, `Print`, `PrintTGA`, `CreateSurfaces`, `CreateDDSurface`, `ConfirmDX6` |
| **DXInput**      | 37 | DirectInput 8 keyboard / joystick / mouse + FFB + **replay codec** | `GetKB`, `GetMouse`, `GetJS`, `CheckKey`, `Configure`, `PlayEffect`, `EnumerateEffects`, `SetEffect`, `Write`, `Read`, `WriteOpen`/`ReadOpen`/`WriteClose` (= input recording stream) |
| **DXPlay**       | 18 | DirectPlay 6 session/lockstep — DXPTYPE protocol host | `NewSession`, `JoinSession`, `SealSession`, `EnumerateSessions`, `SendMessageA`, `ReceiveMessage`, `HandlePadHost`, `HandlePadClient`, `ConnectionEnumerate`/`Pick`, `UnSync` |
| **DXSound**      | 32 | DirectSound 7 + CD audio + streaming | `Play`(×2 overloads), `Stop`, `Status`, `Modify`, `ModifyOveride`, `SetVolume`, `MuteAll`/`UnMuteAll`, `Load`/`LoadBuffer`(×2), `PlayStream`/`StopStream`, `Refresh`, `CDPlay`/`CDStop`/`CDPause`/`CDResume`/`CDReplay`/`CDSetVolume`/`CDGetVolume`, `SetPlayback`, `CanDo3D` |
| **DXWin**        |  7 | Win32 message-pump + DXInitialize/Uninitialize | `Initialize`, `Uninitialize`, `DXInitialize`, `Pause`, `CleanUpAndPostQuit` |
| **DX** (top)     | 29 | Cross-cutting utilities: `Allocate`/`DeAllocate`, `BitCount`, `FOpen`/`FClose`/`FRead`/`FWrite`/`FSeek`/`FSize`, `TGACompress`, `Decode{Bmp,Tga,Rgbs16/24/32}ToScratchSurface`, `ExtractScreenInfo`/`ExtractSystemInfo`, `GetStateString`, `Image`, `LastError`, `info`, `app` |
| **Global stubs** | 15 | Decimal/Hex formatters, `Msg`/`ReleaseMsg`/`Report`/`LogReport`/`DXErrorToString`, `DXGetMainWindowHandle`, `entry` |

**Notable absence**: there is **no separate Replay namespace**. What the project memory calls "replay buffer inside M2DX" lives entirely in **DXInput::Write / Read / WriteOpen / ReadOpen / WriteClose**, plus a globals block at `0x10033ba8`. See Section D.

---

## Section C: API surface — port-side equivalents

| M2DX subsystem | Original API surface | Port-side equivalent | Status |
|----------------|---------------------|----------------------|--------|
| **DXD3D / DXDraw / DXD3DTexture** | DirectDraw 7 + D3D3 immediate-mode (~53 exports) | `ddraw_wrapper/*` (D3D11 backend) + `td5_render.c` | Replaced wholesale. Port does not load M2DX. |
| **DXInput (input poll)** | `GetKB`/`GetJS`/`GetMouse`/`GetAnsiKey` etc. | `td5_input.c` (DirectInput8 polling + INI mapping) | Re-implemented. |
| **DXInput (FFB)** | `EnumerateEffects`/`SetEffect`/`PlayEffect`/`StopEffects`, `FFGainScale` global | `td5_input.c` force-feedback path | Implemented (re-implementation, not byte-faithful). |
| **DXInput (replay codec)** | `WriteOpen`/`Write`/`WriteClose` capture + `ReadOpen`/`Read` playback over a compressed delta stream | **Not ported** (no replay save/load in port). `[[todo-view-replay-restarts-race]]` open. | **Gap**. |
| **DXPlay (networking)** | DirectPlay 6 session enumerate/create/join/seal + `SendMessageA(DXPTYPE,…)` switch with cases 0/1/2/3/4/7/8/9/10, ack-tracked broadcast, host migration via `RefreshCurrentSessionRoster` + `EnumSessionTimer` | `td5_net.c` (flattened DXPTYPE dispatch table, 13 top-level handlers) | **Wire-incompatible.** See `reference_arch_dxptype_protocol_divergence_2026-05-20.md`. |
| **DXSound (sound buffers)** | `Create`, `Load(×2 overloads)`, `LoadBuffer(×2)`, `Play(×2)`, `Stop`, `Modify`/`ModifyOveride`, `SetVolume`/`MuteAll`/`UnMuteAll`, `Refresh`, slot-duplicate fan-out (`g_soundDuplicateCountTable`) | `td5_sound.c` (DirectSound8 via DSCBSOUND wrapper) | Re-implemented; **slot-duplicate cloning** for polyphony likely not modeled. |
| **DXSound (CD audio)** | `CDPlay`/`CDStop`/`CDPause`/`CDResume`/`CDReplay`/`CDSetVolume`/`CDGetVolume` + `InitializeCDAudioBackendFromInstallSource` (looks at install source for tracks 2–N) | `td5_sound.c` music-track stub (silent) | **Gap.** Music tracks not implemented. |
| **DXSound (streaming)** | `PlayStream`/`StopStream` (background WAV streaming) | Not implemented in port | **Gap.** |
| **DXWin** | `Initialize`/`Uninitialize`/`Pause`/`CleanUpAndPostQuit` over the singleton `DX::app` struct | `main.c` win32 bootstrap | Replaced. |
| **DX::file IO** | Wrappers around CRT_fopen/fread/etc. (`FOpen`/`FRead`/`FCount`/`FileENP`) | Direct CRT calls + `td5_asset.c` ZIP reader | Re-implemented. |
| **DX::image decoders** | `DecodeBmpToScratchSurface`, `DecodeTgaToScratchSurface`, `DecodeRgbs{16,24,32}ToScratchSurface`, `ImageProRGB` | `td5_asset.c` TGA decode | TGA covered; BMP/RGBS variants likely unused at runtime. |
| **DX::diagnostic** | `Msg`/`Report`/`LogReport`/`DXErrorToString`/`LastError`/`MessageC` | `OutputDebugStringA` + project log harness | Re-implemented. |

---

## Section D: Highest-value follow-up targets

Five deep-dive functions that, if fully decompiled with field-level globals notation, would close the biggest port-side gaps:

1. **`DXInput::Write` @ `0x1000a660`** (and its sibling `DXInput::Read` @ `0x1000a780`). Confirmed format from decomp: two packed 32-bit input words per frame, delta-encoded into a `(frame_cursor, value)` entry table at `0x10033ba8` with up to **0x4e1c entries × 0x7fee frames** (≈4 minutes at 80 Hz). The high bit `0x8000` on the cursor tags the second word. **`param_2 == 1` strips bits `0x44000000` from both words** (= filters out cosmetic-only input bits during demo capture). This is the entire replay file format. Closes `[[todo-view-replay-restarts-race]]` and unlocks a real replay export feature.

2. **`DXPlay::SendMessageA` @ `0x1000b2a0`**. The DXPTYPE switch with full payload semantics: case 1/3 = small payload to one slot (`DAT_1005b158`), case 2/8 = bigger payload to `g_directPlayOutboundMessagePayload`, **case 4 = ack-tracked per-peer broadcast** using `g_directPlayExpectedAckCount`/`g_directPlayPendingAckCount` over the player-id ring at `0x1005e5c0+0x83`, case 10 = ack-tracked global broadcast over the ring at `0x1005e7e4-0x83`. This is the lockstep barrier mechanism. Pair with `DXPlay::ReceiveMessage` @ `0x1000b580` to fully reconstruct the wire format and audit `td5_net.c`'s flattened 13-handler dispatch table.

3. **`DXPlay::HandlePadHost` @ `0x1000b680`** + **`HandlePadClient` @ `0x1000b8b0`**. These are the per-frame controller-state replication paths called from the game's main tick. Reading them tells us **what fields of the input record propagate over the wire each frame** vs. what is stripped — this directly intersects with the replay codec's `param_2==1` bit-mask above.

4. **`DXSound::Create` @ `0x1000ce30`** (already partially decoded) + **`InitializeCDAudioBackendFromInstallSource`** (referenced from it). The CD-audio backend lives behind `g_pCDAudioBackend` and is **initialized only when the install-source disc is present**. Understanding the backend interface would let us implement Music Tracks from a ripped audio source. Combine with `DXSound::CDPlay` @ `0x1000d8a0` and `DXSound::CDReplay` @ `0x1000dc10` (NB: "CDReplay" = play-same-track-again, not input replay) to enumerate the small vtable in `g_pCDAudioBackend`.

5. **`DXSound::Play` @ `0x1000d380`** + **`Play(overload-2)` @ `0x1000d470`** + the **slot-duplicate fan-out** via `g_soundDuplicateCountTable`/`DAT_1005f9c8`. Confirmed from decomp: each sound slot can clone N times to allow polyphony of the same SFX. The port likely has hard-clipping when the same engine/skid SFX retriggers; this would close the "engine sound choppy" class of port artifacts and is a small, surgical change.

### Honorable mentions (defer)

- `DXPlay::NewSession` @ `0x1000aed0` — full session-init flow; useful only if multiplayer interop is in scope (it isn't per memory note).
- `DXSound::PlayStream` @ `0x1000dd50` / `StopStream` @ `0x1000de70` — background WAV streaming, used for ambient/music. Lower priority than music-tracks (#4).
- `DXD3DTexture::Load` @ `0x10005270` — texture variant of the asset loader; texture path is fully reimplemented in `td5_asset.c`, audit only if a specific texture bug surfaces.

---

## Cleanup

Pool slot 14 was acquired via `ghidra_pool.sh acquire`. Releasing now:

```
bash scripts/ghidra_pool.sh cleanup
```

(Read-only sessions; no transactions opened.)
