# DXPlay & DXD3DTexture Subsystem Mapping — 2026-03-12

## Binary: M2DX.DLL (base 0x10000000)

---

## A. DXPlay — Network / Multiplayer Subsystem

### Exports

| # | VA | Name | Purpose |
|---|---|---|---|
| 1 | `0x1000ab20` | Environment | Zeros 0x2FA dwords at `0x1005dc58` + 0x304 dwords at `0x1005ecd8` |
| 2 | `0x1000ab50` | Create | `CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay4A)` → `g_pDirectPlay`; calls `StartDirectPlayWorker` |
| 3 | `0x1000abd0` | Destroy | Calls `ShutdownDirectPlayWorker` |
| 4 | `0x1000abf0` | Lobby | Stub — returns 0 (shares address with `DXSound::CanDo3D`) |
| 5 | `0x1000ac00` | ConnectionEnumerate | Enumerates service providers via `IDP4::EnumConnections` (vtable+0x8C) |
| 6 | `0x1000add0` | ConnectionPick | `IDP4::InitializeConnection` (vtable+0x98); starts 3-second session enum timer |
| 7 | `0x1000aed0` | NewSession | `IDP4::Open` (vtable+0x60) host mode; `CreatePlayer` (vtable+0x18); `RefreshCurrentSessionRoster` |
| 8 | `0x1000b0d0` | JoinSession | Joins enumerated session by index; `CreatePlayer`; `RefreshCurrentSessionRoster` |
| 9 | `0x1000b2a0` | SendMessageA | Switch on DXPTYPE (0–10); `IDP4::Send` (vtable+0x68) |
| 10 | `0x1000b580` | ReceiveMessage | Dequeues from ring buffer at `0x1005b398`; stride 0x244; 16 entries |
| 11 | `0x1000b680` | HandlePadHost | Host frame sync: waits on sync/frame-ack events; broadcasts frame to all players |
| 12 | `0x1000b8b0` | HandlePadClient | Client frame sync: sends input to host; handles host migration |
| 13 | `0x1000bb40` | EnumerateSessions | `IDP4::EnumSessions` (vtable+0x34); caches GUIDs at `0x1005df54` |
| 14 | `0x1000bc40` | SealSession | `IDP4::SetSessionDesc` (vtable+0x7C); toggles open/close flag (0x44↔0x45) |
| 15 | `0x1000bca0` | EnumSessionTimer | `SetTimer` / `KillTimer` for periodic session enumeration (3000 ms) |
| 16 | `0x1000bd00` | UnSync | Clears `g_isDirectPlaySyncActive` |
| D1 | `0x1005b148` | bConnectionLost | DWORD flag — set on session-loss system message |
| D2 | `0x1005ec4c` | bEnumerateSessionBusy | DWORD flag — guards reentrant enum |
| D3 | `0x1005ecd8` | dpu | 0x304-DWORD struct — per-unit data block |

### Internal Helpers

| VA | Proposed Name | Purpose |
|---|---|---|
| `0x1000bfb0` | StartDirectPlayWorker | Creates 4 Win32 events + worker thread |
| `0x1000c050` | ShutdownDirectPlayWorker | Signals thread stop; `CloseHandle` ×5; `IDP4::Release` (vtable+0x08) |
| `0x1000c0e0` | DirectPlayWorkerThread | Main loop: `WaitForMultipleObjects` on 4 events; dispatches `IDP4::Receive` → ring buffer |
| `0x1000c4d0` | HandleDirectPlayAppMessage | Switch on 13 application message types (0–12) |
| `0x1000c9a0` | HandleDirectPlaySystemMessage | Handles host migration (0x101), session loss (0x31), player delete (0x21) |
| `0x1000bd10` | RefreshCurrentSessionRoster | `IDP4::EnumPlayers` (vtable+0x30); rebuilds player ID/name/slot tables |

### COM Identity

| Item | Value |
|---|---|
| CLSID_DirectPlay | `{D1EB6D20-8923-11D0-9D97-00A0C90A43CB}` |
| IID_IDirectPlay4A | `{0AB1C531-4745-11D1-A7A1-0000F803ABFC}` |

### DirectPlay4A vtable offsets used

| Offset | Method |
|---|---|
| +0x08 | Release |
| +0x10 | Close |
| +0x18 | CreatePlayer |
| +0x24 | DestroyPlayer |
| +0x30 | EnumPlayers |
| +0x34 | EnumSessions |
| +0x54 | GetPlayerData |
| +0x58 | GetSessionDesc |
| +0x60 | Open |
| +0x68 | Send |
| +0x78 | SetPlayerData |
| +0x7C | SetSessionDesc |
| +0x8C | EnumConnections |
| +0x98 | InitializeConnection |

### Key Globals

| VA | Type | Proposed Name | Notes |
|---|---|---|---|
| `0x1005dc58` | IDirectPlay4A* | g_pDirectPlay | Set by `Create`, released by `ShutdownDirectPlayWorker` |
| `0x1005b398` | byte[0x2440] | g_messageRingBuffer | 16 entries × 0x244 bytes each |
| `0x1005f8e8` | int | g_messageQueueReadIndex | ReceiveMessage consumer |
| `0x1005ecd0` | int | g_messageQueueWriteIndex | Worker thread producer |
| `0x1005b150` | int | g_outboundMessageType | DXPTYPE field for SendMessageA |
| `0x1005b148` | int | g_bConnectionLost | Exported data; set on DPNMSG_SESSIONLOST |
| `0x1005ec4c` | int | g_bEnumerateSessionBusy | Exported data; guards reentrant EnumSessions |
| `0x1005ecd8` | byte[0xC10] | g_dpu | Exported data; per-unit struct block |
| `0x1005df54` | GUID[N] | g_enumeratedSessionGUIDs | stride 0x10; cached by EnumerateSessions |
| `0x1005e418` | int | g_enumeratedSessionCount | Updated by EnumerateSessions callback |
| `0x1005e7d0` | DPID[6] | g_playerIDs | Player ID table (6 slots) |
| `0x1005e5c0` | int[6] | g_playerActiveFlags | 1 = active, 0 = empty |
| `0x1005dc5c` | HANDLE | g_hWorkerThread | Worker thread handle |
| `0x1005dc60` | HANDLE | g_hEventStop | Signals worker to exit |
| `0x1005dc64` | HANDLE | g_hEventSync | Frame sync event |
| `0x1005dc68` | HANDLE | g_hEventFrameAck | Frame acknowledgement event |
| `0x1005dc6c` | HANDLE | g_hEventReceive | Data-available event |

### DXPTYPE Protocol Enum

SendMessageA switches on message type 0–10:

| Value | Meaning (inferred) |
|---|---|
| 0 | Raw data / generic |
| 1 | Pad input frame |
| 2 | Sync request |
| 3 | Sync acknowledge |
| 4 | Frame data (host → clients) |
| 5 | Frame acknowledge (client → host) |
| 6–10 | Extended game-state messages |

### Worker Thread Architecture

1. **`Create`** → `CoCreateInstance` → `StartDirectPlayWorker`
2. **`StartDirectPlayWorker`** creates 4 events (Stop, Sync, FrameAck, Receive) + spawns `DirectPlayWorkerThread`
3. **`DirectPlayWorkerThread`** loops on `WaitForMultipleObjects(4, events)`:
   - On Receive event: calls `IDP4::Receive(DPID_SYSMSG)` in loop; for each message dispatches to `HandleDirectPlayAppMessage` or `HandleDirectPlaySystemMessage`
   - Pushes decoded messages into ring buffer at `g_messageRingBuffer[writeIndex % 16]`
4. **`HandlePadHost`** / **`HandlePadClient`** run on the game thread:
   - Host: waits on sync+frame-ack events, broadcasts consolidated frame
   - Client: sends local input, waits for host frame, handles host migration if host disconnects
5. **`ShutdownDirectPlayWorker`** signals stop event, waits for thread, closes all handles, releases COM object

### Host Migration

Handled in `HandleDirectPlaySystemMessage` (0x1000c9a0):
- System message `0x101` (DPSYS_HOST) → current player becomes new host
- System message `0x31` (DPSYS_SESSIONLOST) → sets `g_bConnectionLost = 1`
- System message `0x21` (DPSYS_DELETEPLAYER) → clears player slot from roster

---

## B. DXD3DTexture — Managed Texture Subsystem

### Exports

| # | VA | Name | Purpose |
|---|---|---|---|
| 1 | `0x100051b0` | Environment | Zeros 0x13A dwords at `0x100299f0` (format descriptor + texture arrays) |
| 2 | `0x100051d0` | Create | Allocates 0x14000-byte file buffer + aux buffer; seeds image-line workspace (0x18 dwords at `0x10029560`) |
| 3 | `0x10005240` | Destroy | Frees file/aux buffers; resets `g_textureCount` to 0 |
| 4 | `0x10005270` | Load | Classifies by extension: `.rgb`→1, `.bmp`→2, `.tga`→3; reads file into buffer; sets `g_textureLoadSourceType`; calls `Manage` |
| 5 | `0x100053e0` | LoadRGB | In-memory RGB load; stores width/height/pointer metadata; `sourceType=1`; calls `Manage` |
| 6 | `0x10005480` | LoadRGBS24 | In-memory 24-bit RGB load; `sourceType=4`; calls `Manage` |
| 7 | `0x100054e0` | LoadRGBS32 | In-memory 32-bit RGBA load; `sourceType=5`; calls `Manage` |
| 8 | `0x10005550` | Manage | Main dispatcher: switches on `g_textureLoadSourceType` (0–5); creates scratch surface; decodes image; creates D3D texture; stores in texture array |
| 9 | `0x10005ba0` | GetMask | Returns R/G/B/A bitmasks from selected format slot |
| 10 | `0x10005ea0` | LoseAll | Releases all `IDirect3DTexture2*` interfaces; preserves metadata for `RestoreAll` |
| 11 | `0x10005f10` | RestoreAll | Rebuilds textures from metadata: file-backed by re-loading filename, memory-backed from stored pointer |
| 12 | `0x10006010` | ClearAll | Releases all textures + clears count (no restore capability) |
| D1 | `0x100299c8` | Set | `tagTextureSet` — global texture set descriptor |
| D2 | `0x10029f60` | Texture | `tagDXD3DTex[]` — per-texture record array |

### Internal Helpers

| VA | Proposed Name | Purpose |
|---|---|---|
| `0x10004410` | EnumerateTextureFormats | `IDirect3DDevice::EnumTextureFormats(ClassifyCallback, &selectionSlots)` |
| `0x10004580` | ClassifyTextureFormatDescriptor | Callback: stores `DDPIXELFORMAT`; classifies into pal4/8, 16/32-bit alpha categories; promotes best format indices |
| `0x10005c40` | SelectTextureFormatAndLockScratchSurface | Chooses format slot by image type; creates scratch `IDirectDrawSurface`; locks it for pixel transfer |

### Per-Texture Record Layout

Base: `0x10029f60` (`Texture` export) — stride **0x30 (48 bytes)**

```
Offset  Size  Type                   Field
──────  ────  ─────────────────────  ──────────────────────────
+0x00   4     IDirect3DTexture2*     pTexture
+0x04   4     int                    paletteEntryCount
+0x08   16    char[16]               filename (empty for memory-backed)
+0x18   4     void*                  pSourceData (for RestoreAll rebuild)
+0x1C   4     int                    slotIndex (back-reference into array)
+0x20   4     int                    imageType (IMAGETYPE enum)
+0x24   4     int                    width
+0x28   4     int                    height
+0x2C   4     int                    createFlags
```

**IMAGETYPE enum (inferred):**

| Value | Meaning |
|---|---|
| 1 | RGB (Silicon Graphics .rgb format) |
| 2 | BMP (Windows bitmap) |
| 3 | TGA (Targa) |
| 4 | RGBS24 (raw 24-bit in-memory) |
| 5 | RGBS32 (raw 32-bit in-memory) |

### Texture Format Selection Pipeline

#### Step 1: Enumeration (`EnumerateTextureFormats`, 0x10004410)

Calls `IDirect3DDevice::EnumTextureFormats` with `ClassifyTextureFormatDescriptor` as callback.

#### Step 2: Classification (`ClassifyTextureFormatDescriptor`, 0x10004580)

For each `DDPIXELFORMAT` reported by the driver:
1. Stores full descriptor in table at `0x100299f0` (stride 0x38)
2. Extracts per-channel bit counts (R, G, B, A)
3. Classifies into one of 9 categories:

| Slot | Class Code | Category |
|---|---|---|
| 0 | 1 | Palettized 4-bit |
| 1 | 2 | Palettized 8-bit |
| 2 | 3 | 16-bit, no alpha |
| 3 | 4 | 16-bit, 1-bit alpha |
| 4 | 5 | 16-bit, multi-bit alpha |
| 5 | 6 | 32-bit, no alpha |
| 6 | 7 | 32-bit, 1-bit alpha |
| 7 | 8 | 32-bit, multi-bit alpha |

4. Promotes "best" format per slot (prefers higher bit depth within category)

#### Step 3: Selection (`SelectTextureFormatAndLockScratchSurface`, 0x10005c40)

At texture load time:
1. Reads `imageType` to determine desired format category
2. Picks best available slot from `g_textureFormatSelectionSlots`
3. Creates a scratch `IDirectDrawSurface` matching the chosen pixel format
4. Locks the surface → returns locked pointer for pixel transfer

#### Step 4: Transfer (inside `Manage`, 0x10005550)

1. Decodes source image (RGB/BMP/TGA/raw) into scratch surface
2. Calls `IDirectDrawSurface::QueryInterface` for `IDirect3DTexture2`
3. Stores `IDirect3DTexture2*` in texture array slot
4. Increments `g_textureCount`

### Key Globals

| VA | Type | Proposed Name | Notes |
|---|---|---|---|
| `0x100294d4` | void* | g_textureFileBuffer | 0x14000-byte buffer for file I/O |
| `0x100294d0` | void* | g_textureAuxBuffer | Auxiliary decode buffer |
| `0x10029e8c` | int | g_textureCount | Current number of loaded textures |
| `0x10030fe8` | int | g_textureLoadSourceType | Set before `Manage`: 0–5 (IMAGETYPE) |
| `0x10029e88` | int | g_textureFormatDescriptorCount | Number of formats enumerated |
| `0x10029e90` | int[18] | g_textureFormatSelectionSlots | 9 pairs of {descriptorIndex, classCode} |
| `0x100299f0` | byte[] | g_textureFormatDescriptorTable | stride 0x38; DDPIXELFORMAT + bit counts |
| `0x100299e8` | IDirectDrawSurface* | g_pTextureScratchSurface | Temporary surface for pixel transfer |
| `0x10029560` | int[0x18] | g_imageLineWorkspace | Per-scanline decode workspace |
| `0x10028a90` | IDirect3DDevice* | g_pDirect3DDevice | Used by EnumerateTextureFormats |
| `0x100299c8` | tagTextureSet | g_textureSet | Exported `Set` descriptor |
| `0x10029f60` | tagDXD3DTex[] | g_textureArray | Exported `Texture` array (stride 0x30) |

### LoseAll / RestoreAll Protocol

**`LoseAll` (0x10005ea0):**
- Iterates `g_textureArray[0..count-1]`
- Calls `IDirect3DTexture2::Release` on each `pTexture`
- Sets `pTexture = NULL` but preserves all other metadata fields
- Called on device loss (e.g., Alt+Tab, mode switch)

**`RestoreAll` (0x10005f10):**
- Iterates `g_textureArray[0..count-1]`
- If `filename[0] != '\0'`: calls `Load(filename)` to rebuild from file
- Else if `pSourceData != NULL`: calls appropriate `LoadRGB*` using stored pointer + dimensions
- Restores `pTexture` pointer in-place

**`ClearAll` (0x10006010):**
- Releases all textures AND zeros metadata
- Resets `g_textureCount = 0`
- No restore possible after this call

---

## Proposed Renames

| Address | Binary | Current Name | Proposed Name | Confidence | Evidence |
|---|---|---|---|---|---|
| `0x1000ab20` | M2DX.DLL | `FUN_1000ab20` | `DXPlay::Environment` | confirmed | Export name from DLL |
| `0x1000ab50` | M2DX.DLL | `FUN_1000ab50` | `DXPlay::Create` | confirmed | `CoCreateInstance(CLSID_DirectPlay)` |
| `0x1000abd0` | M2DX.DLL | `FUN_1000abd0` | `DXPlay::Destroy` | confirmed | Calls `ShutdownDirectPlayWorker` |
| `0x1000abf0` | M2DX.DLL | `FUN_1000abf0` | `DXPlay::Lobby` | confirmed | Stub, returns 0 |
| `0x1000ac00` | M2DX.DLL | `FUN_1000ac00` | `DXPlay::ConnectionEnumerate` | confirmed | `IDP4::EnumConnections` |
| `0x1000add0` | M2DX.DLL | `FUN_1000add0` | `DXPlay::ConnectionPick` | confirmed | `IDP4::InitializeConnection` + timer |
| `0x1000aed0` | M2DX.DLL | `FUN_1000aed0` | `DXPlay::NewSession` | confirmed | `IDP4::Open` host mode |
| `0x1000b0d0` | M2DX.DLL | `FUN_1000b0d0` | `DXPlay::JoinSession` | confirmed | Joins by session index |
| `0x1000b2a0` | M2DX.DLL | `FUN_1000b2a0` | `DXPlay::SendMessageA` | confirmed | Switch on DXPTYPE, `IDP4::Send` |
| `0x1000b580` | M2DX.DLL | `FUN_1000b580` | `DXPlay::ReceiveMessage` | confirmed | Ring buffer dequeue |
| `0x1000b680` | M2DX.DLL | `FUN_1000b680` | `DXPlay::HandlePadHost` | confirmed | Host frame sync pattern |
| `0x1000b8b0` | M2DX.DLL | `FUN_1000b8b0` | `DXPlay::HandlePadClient` | confirmed | Client frame sync + host migration |
| `0x1000bb40` | M2DX.DLL | `FUN_1000bb40` | `DXPlay::EnumerateSessions` | confirmed | `IDP4::EnumSessions` |
| `0x1000bc40` | M2DX.DLL | `FUN_1000bc40` | `DXPlay::SealSession` | confirmed | Toggles session open flag |
| `0x1000bca0` | M2DX.DLL | `FUN_1000bca0` | `DXPlay::EnumSessionTimer` | confirmed | `SetTimer`/`KillTimer` 3000ms |
| `0x1000bd00` | M2DX.DLL | `FUN_1000bd00` | `DXPlay::UnSync` | confirmed | Clears sync-active flag |
| `0x1000bd10` | M2DX.DLL | `FUN_1000bd10` | `RefreshCurrentSessionRoster` | confirmed | `IDP4::EnumPlayers` → rebuilds tables |
| `0x1000bfb0` | M2DX.DLL | `FUN_1000bfb0` | `StartDirectPlayWorker` | confirmed | Creates events + thread |
| `0x1000c050` | M2DX.DLL | `FUN_1000c050` | `ShutdownDirectPlayWorker` | confirmed | Stops thread, closes handles |
| `0x1000c0e0` | M2DX.DLL | `FUN_1000c0e0` | `DirectPlayWorkerThread` | strongly-suspected | WFMO loop, Receive dispatch |
| `0x1000c4d0` | M2DX.DLL | `FUN_1000c4d0` | `HandleDirectPlayAppMessage` | strongly-suspected | Switch on 13 app msg types |
| `0x1000c9a0` | M2DX.DLL | `FUN_1000c9a0` | `HandleDirectPlaySystemMessage` | confirmed | Host migration (0x101), session loss (0x31) |
| `0x1005dc58` | M2DX.DLL | `DAT_1005dc58` | `g_pDirectPlay` | confirmed | Set by `CoCreateInstance` in `Create` |
| `0x1005b398` | M2DX.DLL | `DAT_1005b398` | `g_messageRingBuffer` | confirmed | 16×0x244 ring, read/write indices |
| `0x1005f8e8` | M2DX.DLL | `DAT_1005f8e8` | `g_messageQueueReadIndex` | confirmed | Consumed in `ReceiveMessage` |
| `0x1005ecd0` | M2DX.DLL | `DAT_1005ecd0` | `g_messageQueueWriteIndex` | confirmed | Produced in worker thread |
| `0x1005b148` | M2DX.DLL | `DAT_1005b148` | `g_bConnectionLost` | confirmed | Exported data symbol |
| `0x1005ec4c` | M2DX.DLL | `DAT_1005ec4c` | `g_bEnumerateSessionBusy` | confirmed | Exported data symbol |
| `0x1005df54` | M2DX.DLL | `DAT_1005df54` | `g_enumeratedSessionGUIDs` | strongly-suspected | stride 0x10 GUID table |
| `0x1005e418` | M2DX.DLL | `DAT_1005e418` | `g_enumeratedSessionCount` | strongly-suspected | Updated in enum callback |
| `0x1005e7d0` | M2DX.DLL | `DAT_1005e7d0` | `g_playerIDs` | strongly-suspected | 6×DPID array |
| `0x1005e5c0` | M2DX.DLL | `DAT_1005e5c0` | `g_playerActiveFlags` | strongly-suspected | 6×int active flags |
| `0x100051b0` | M2DX.DLL | `FUN_100051b0` | `DXD3DTexture::Environment` | confirmed | Export name |
| `0x100051d0` | M2DX.DLL | `FUN_100051d0` | `DXD3DTexture::Create` | confirmed | Allocates buffers |
| `0x10005240` | M2DX.DLL | `FUN_10005240` | `DXD3DTexture::Destroy` | confirmed | Frees buffers |
| `0x10005270` | M2DX.DLL | `FUN_10005270` | `DXD3DTexture::Load` | confirmed | File extension dispatch |
| `0x100053e0` | M2DX.DLL | `FUN_100053e0` | `DXD3DTexture::LoadRGB` | confirmed | In-memory RGB, type=1 |
| `0x10005480` | M2DX.DLL | `FUN_10005480` | `DXD3DTexture::LoadRGBS24` | confirmed | In-memory 24-bit, type=4 |
| `0x100054e0` | M2DX.DLL | `FUN_100054e0` | `DXD3DTexture::LoadRGBS32` | confirmed | In-memory 32-bit, type=5 |
| `0x10005550` | M2DX.DLL | `FUN_10005550` | `DXD3DTexture::Manage` | confirmed | Central texture creation dispatcher |
| `0x10005ba0` | M2DX.DLL | `FUN_10005ba0` | `DXD3DTexture::GetMask` | confirmed | Returns R/G/B/A bitmasks |
| `0x10005ea0` | M2DX.DLL | `FUN_10005ea0` | `DXD3DTexture::LoseAll` | confirmed | Release interfaces, keep metadata |
| `0x10005f10` | M2DX.DLL | `FUN_10005f10` | `DXD3DTexture::RestoreAll` | confirmed | Rebuilds from metadata |
| `0x10006010` | M2DX.DLL | `FUN_10006010` | `DXD3DTexture::ClearAll` | confirmed | Full release + zero |
| `0x10004410` | M2DX.DLL | `FUN_10004410` | `EnumerateTextureFormats` | confirmed | `IDirect3DDevice::EnumTextureFormats` |
| `0x10004580` | M2DX.DLL | `FUN_10004580` | `ClassifyTextureFormatDescriptor` | strongly-suspected | Format classification callback |
| `0x10005c40` | M2DX.DLL | `FUN_10005c40` | `SelectTextureFormatAndLockScratchSurface` | strongly-suspected | Format selection + scratch surface |
| `0x100294d4` | M2DX.DLL | `DAT_100294d4` | `g_textureFileBuffer` | confirmed | 0x14000-byte alloc in Create |
| `0x100294d0` | M2DX.DLL | `DAT_100294d0` | `g_textureAuxBuffer` | confirmed | Aux alloc in Create |
| `0x10029e8c` | M2DX.DLL | `DAT_10029e8c` | `g_textureCount` | confirmed | Incremented per Manage call |
| `0x10030fe8` | M2DX.DLL | `DAT_10030fe8` | `g_textureLoadSourceType` | confirmed | Set before Manage dispatch |
| `0x10029e88` | M2DX.DLL | `DAT_10029e88` | `g_textureFormatDescriptorCount` | strongly-suspected | Count of enumerated formats |
| `0x10029e90` | M2DX.DLL | `DAT_10029e90` | `g_textureFormatSelectionSlots` | confirmed | 9×{index,class} pairs |
| `0x100299f0` | M2DX.DLL | `DAT_100299f0` | `g_textureFormatDescriptorTable` | confirmed | stride 0x38 |
| `0x100299e8` | M2DX.DLL | `DAT_100299e8` | `g_pTextureScratchSurface` | strongly-suspected | Temp surface for pixel xfer |
| `0x10028a90` | M2DX.DLL | `DAT_10028a90` | `g_pDirect3DDevice` | confirmed | Used by EnumerateTextureFormats |
