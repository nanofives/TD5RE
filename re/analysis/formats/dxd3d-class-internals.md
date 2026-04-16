# DXD3D Class Internals -- M2DX.DLL Deep Dive

> Binary: M2DX.DLL | Ghidra port 8192
> Analysis date: 2026-03-19

## Class Architecture

M2DX.DLL implements five major subsystem classes, all using static methods (no `this` pointer, pure `__cdecl`):

| Class | Address Range | Role |
|---|---|---|
| **DXD3D** | 0x10001000 - 0x10002470 | D3D device lifecycle, render state, scene management |
| **DXD3DTexture** | 0x100051b0 - 0x10006090 | Texture loading, format selection, device-loss recovery |
| **DXDraw** | 0x100061e0 - 0x10009380 | DirectDraw surfaces, flip/present, frame rate, overlays |
| **DXInput** | 0x100094e0 - 0x1000aaf0 | Keyboard, joystick, force feedback |
| **DXSound** | 0x1000cda0 - 0x1000d370 | DirectSound buffers |

There is **no vtable** for any of these classes. All methods are static, exported by mangled name. The "class" is purely a namespace grouping.

---

## DXD3D Exported Methods (Complete)

| VA | Export | Signature | Purpose |
|---|---|---|---|
| 0x10001000 | `DXD3D::Environment` | `int __cdecl()` | Zero-init 0xA60-byte driver workspace, seed default render states |
| 0x100010b0 | `DXD3D::Create` | `int __cdecl()` | Create IDirect3D3, enumerate device drivers, bring up renderer |
| 0x100011f0 | `DXD3D::Destroy` | `int __cdecl()` | Release viewport callback, textures, device, IDirect3D3 |
| 0x10001270 | `DXD3D::InitializeMemoryManagement` | `int __cdecl()` | Probe VRAM via GetAvailableVidMem, compute surface budget, filter display modes |
| 0x10001580 | `DXD3D::CanFog` | `int __cdecl()` | Returns `g_d3dDriverFogSupportTable[activeDriver]` |
| 0x100015a0 | `DXD3D::BeginScene` | `int __cdecl()` | Restore lost surfaces, clear viewport, IDirect3DDevice3::BeginScene |
| 0x100016b0 | `DXD3D::EndScene` | `int __cdecl()` | IDirect3DDevice3::EndScene, then DXDraw::Flip |
| 0x10001700 | `DXD3D::TextureClamp` | `int __cdecl(int)` | Set texture address mode: 0=WRAP, nonzero=CLAMP |
| 0x10001770 | `DXD3D::SetRenderState` | `int __cdecl()` | Apply full baseline render state block |
| 0x10001ab0 | `DXD3D::GetStats` | `void __cdecl(D3DSTATS*)` | IDirect3DDevice3::GetStats wrapper |
| 0x10001ad0 | `DXD3D::ChangeDriver` | `int __cdecl(int)` | Hot-swap to a different D3D driver index at runtime |
| 0x10001d40 | `DXD3D::GetMaxTextures` | `int __cdecl(int)` | Estimate streaming texture capacity from VRAM budget |
| 0x10002170 | `DXD3D::FullScreen` | `int __cdecl(int)` | Switch to fullscreen: tear down all surfaces, rebuild at new resolution |

### Exported Global

| VA | Export | Type |
|---|---|---|
| 0x100314d8 | `DXD3D::bResetResolution` | `int` (flag set when surface restore fails after WM_SIZE) |

---

## Internal (Non-Exported) DXD3D Functions

| VA | Name | Purpose |
|---|---|---|
| 0x10002470 | `EnumerateD3DDeviceCaps` | IDirect3D3::EnumDevices callback; populates per-driver capability tables |
| 0x10002920 | `CreateAndBindViewport` | Create IDirect3DViewport3, set D3DVIEWPORT2, identity world transform |
| 0x10002ae0 | `SelectPreferredDisplayMode` | Find best display mode matching a bit-depth mask |
| 0x10002c90 | `ResolveCompatibleD3DDriverModePair` | Resolve sentinel values (-25=auto, -24=windowed) into concrete driver+mode |
| 0x10003320 | `SelectCompatibleD3DDriver` | Pick best driver from record table matching bit-depth mask (prefers HW) |
| 0x100033f0 | `InitializeDriverFeatureStates` | Viewport-create callback: sync subpixel/Z-bias/LOD-bias from driver caps |
| 0x10003490 | `ReleaseViewportAndDeviceObjects` | Viewport-release callback: release viewport + device |
| 0x100034d0 | `InitializeD3DDriverAndMode` | Top-level D3D bring-up: resolve pair, create surfaces, device, viewport, render state |
| 0x10003940 | `ResizeWindowedD3DDevice` | Windowed resize: tear down, rebuild at new size |
| 0x10003c10 | `SelectZBufferFormat` | Enumerate Z-buffer pixel formats, select preferred depth |
| 0x10003e50 | `CreateAndAttachZBuffer` | Create Z-buffer surface, attach to back buffer |
| 0x10004000 | `CreateD3DDeviceForDriverRecord` | Create IDirect3DDevice3 for a GUID, enumerate texture formats |
| 0x10004410 | `EnumerateTextureFormats` | IDirect3DDevice3::EnumTextureFormats entry point |
| 0x10004580 | `ClassifyTextureFormatDescriptor` | EnumTextureFormats callback: classify into 9 format slots |
| 0x10004850 | `RecordZBufferFormatDescriptor` | EnumZBufferFormats callback |
| 0x100048e0 | `HandleDXDrawWindowMessage` | WM_ACTIVATEAPP handler |
| 0x10004bc0 | `HandleRenderWindowResize` | WM_SIZE handler: smart rebuild vs surface restore |
| 0x10004f30 | `InitializeD3DDeviceSurfaces` | Create render target surfaces, Z-buffer format, triple buffer logic |

---

## D3D Device GUIDs (at .rdata)

| Address | GUID | D3D Device Class |
|---|---|---|
| 0x1001d640 | `{BB223240-E72B-11D0-A9B4-00AA00C0993E}` | IID_IDirect3D3 |
| 0x1001d660 | `{A4665C60-2673-11CF-A31A-00AA00B93356}` | IID_IDirect3DRGBDevice (software rasterizer) |
| 0x1001d670 | `{84E63dE0-46AA-11CF-816F-0000C020156E}` | IID_IDirect3DHALDevice (hardware accelerated) |
| 0x1001d680 | `{881949A1-D6F3-11D0-89AB-00A0C9054129}` | IID_IDirect3DMMXDevice (MMX rasterizer) |
| 0x1001d690 | `{50936643-13E9-11D1-89AA-00A0C9054129}` | IID_IDirect3DRefDevice (reference rasterizer) |
| 0x1001d6f0 | `{93281502-8CF8-11D0-89AB-00A0C9054129}` | IID_IDirect3DTexture2 |

Device creation tries GUIDs in order: HAL -> RGB -> MMX -> Ref. Max 5 driver records.

---

## Render State Management

### SetRenderState (0x10001770) -- Baseline State Block

`SetRenderState` is called once per mode switch (Create, FullScreen, Resize). It wraps the entire state setup inside a BeginScene/EndScene pair. The render states use the **IDirect3DDevice3 vtable** (not IDirect3DDevice2), with SetRenderState at vtable offset +0x58 and SetTextureStageState at +0xA0.

**D3DRENDERSTATETYPE mapping** (DX6 values):

| Hex | D3DRENDERSTATE | Value Set | Notes |
|---|---|---|---|
| 0x01 | TEXTUREMAPBLEND | 0 (DISABLE) | When no textures supported |
| 0x04 | ZENABLE | `DAT_1002949c` (1) | Z-buffer on |
| 0x07 | ZWRITEENABLE | `g_activeSubpixelState` | 1 or 2 depending on W-buffer |
| 0x09 | SHADEMODE | `DAT_10028a60` (2=GOURAUD) | |
| 0x0F | CULLMODE | 1 (NONE) | |
| 0x15 | TEXTUREMAPBLEND | 4 (MODULATE) | When textures supported |
| 0x16 | FILLMODE | `DAT_10028a88` (1=POINT) | |
| 0x17 | LINEPATTERN | 4 | |
| 0x18 | ALPHABLENDENABLE | 0 (FALSE) | |
| 0x19 | SRCBLEND | 6 (SRCALPHA) | Applied at end |
| 0x1A | DITHERENABLE | per-driver `g_activeDriverRenderState1A` | |
| 0x1B | SPECULARENABLE | 1 (TRUE) | |
| 0x1C | FOGVERTEXMODE | 1 | (inside fog block) |
| 0x1F | STIPPLEDALPHA | per-driver `g_activeDriverRenderState1F` | |
| 0x21 | TEXTUREMAPBLEND | 2 (DECAL) | When no textures supported |
| 0x22 | FOGCOLOR | `DAT_10028a70` (0xFFFFFF) | |
| 0x23 | FOGTABLEMODE | 3 (LINEAR) | |
| 0x24 | FOGSTART | 0x3C23D70A (0.01f) | |
| 0x25 | FOGEND | 0x3F000000 (0.5f) | |
| 0x26 | FOGDENSITY | 0x3F7D70A4 (0.99f) | |
| 0x29 | COLORKEYENABLE | 0 (FALSE) | |
| 0x2C | TEXTUREADDRESSU | 1=WRAP or 3=CLAMP | via TextureClamp state |
| 0x2D | TEXTUREADDRESSV | 1=WRAP or 3=CLAMP | via TextureClamp state |

**Texture Stage States** (SetTextureStageState, stage 0):

| Hex | D3DTEXTURESTAGESTATETYPE | Value | Notes |
|---|---|---|---|
| 0x10 | MAGFILTER | per-driver table | 1=POINT..5=ANISOTROPIC |
| 0x11 | MINFILTER | per-driver table | |
| 0x12 | MIPFILTER | per-driver table | 1=NONE, 2=POINT, 3=LINEAR |
| 0x13 | MIPMAPLODBIAS | 1 | Only when LOD bias disabled |

### TextureClamp (0x10001700) -- Texture Address Mode

Simple cached toggle:
- `param_1 == 0`: WRAP mode (U=1, V=1) -- D3DTADDRESS_WRAP
- `param_1 != 0`: CLAMP mode (U=3, V=3) -- D3DTADDRESS_CLAMP
- Cached in `DAT_10029490` to avoid redundant state changes

### Fog Configuration

When `DAT_100294b0 != 0` (fog enabled), SetRenderState applies:
- Fog mode: LINEAR (table mode 3)
- Fog color: 0xFFFFFF (white)
- Fog start: 0.01f
- Fog end: 0.5f
- Fog density: 0.99f
- Vertex fog mode: 1

`DXD3D::CanFog()` checks `g_d3dDriverFogSupportTable` (D3DPRIMCAPS.dwMiscCaps & 0x100).

---

## Per-Driver Capability Tables

`EnumerateD3DDeviceCaps` (0x10002470) populates parallel arrays at stride 0x5A dwords per driver record (max 5 records). Each table is indexed as `table[driverIndex * 0x5A]`.

| Global Name | Capability Probed | Source Caps Field |
|---|---|---|
| `g_d3dDriverIsHardwareTable` | Hardware vs software | D3DDEVICEDESC.dcmColorModel != 0 |
| `g_d3dDriverTextureFormatSupportTable` | Texture format enum | dpcTriCaps.dwTextureCaps & 1 |
| `g_d3dDriverZBufferSupportTable` | Z-buffer | dpcTriCaps.dwZBufferBitDepths != 0 |
| `g_d3dDriverSubpixelAccuracyTable` | Subpixel mode | dwDevCaps & 0x40000 |
| `g_d3dDriverLodBiasSupportTable` | LOD bias | dwDevCaps & 0x2000 |
| `g_d3dDriverZBiasSupportTable` | Z-bias | dwDevCaps & 0x4000 |
| `g_d3dDriverRenderState1ATable` | Dithering | dwDevCaps & 0x01 |
| `g_d3dDriverRenderState1FTable` | Stippled alpha | dwDevCaps & 0x20 |
| `g_d3dDriverFogSupportTable` | Fog | dwDevCaps & 0x100 |
| `g_d3dDriverMagFilterModeTable` | Magnification filter | dpcTriCaps.dwTextureFilterCaps bits |
| `g_d3dDriverMinFilterModeTable` | Minification filter | dpcTriCaps.dwTextureFilterCaps bits |
| `g_d3dDriverMipFilterModeTable` | Mipmap filter | dpcTriCaps.dwTextureFilterCaps bits |
| `g_d3dDriverDisplayModeCompatibilityTable` | Bit-depth match | dpcTriCaps.dwRenderBitDepths |
| `g_d3dDriverCompatibleBitDepthMaskTable` | Supported depths | (derived during driver selection) |

### Filter Mode Values

Magnification filter (dwTextureFilterCaps):
- `& 0x01000000` -> 1 (POINT)
- `& 0x02000000` -> 2 (LINEAR/BILINEAR)
- `& 0x08000000` -> 3
- `& 0x10000000` -> 4
- `& 0x04000000` -> 5 (ANISOTROPIC)

Minification filter:
- `& 0x100` -> 1 (POINT)
- `& 0x200` -> 2 (LINEAR)
- `& 0x400` -> 3

Mipmap filter (when mip enabled):
- `& 0x10000` -> 2 (POINT)
- `& 0x20000` -> 3 (LINEAR)

---

## Texture Format Classification

### 9 Format Slots

`ClassifyTextureFormatDescriptor` (0x10004580) classifies enumerated DDPIXELFORMAT descriptors into 9 slots, stored in parallel index/class-code pairs:

| Slot | Class Code | Description | Pixel Format |
|---|---|---|---|
| PAL4 | 1 | 4-bit palettized | DDPF_PALETTEINDEXED4 (flag 0x08) |
| PAL8 | 2 | 8-bit palettized | DDPF_PALETTEINDEXED8 (flag 0x20) |
| 16-NoAlpha | 3 | 16bpp opaque (e.g. RGB565) | DDPF_RGB, 16bpp, alpha bits=0 |
| 16-OneBitAlpha | 4 | 16bpp 1-bit alpha (e.g. ARGB1555) | DDPF_RGB, 16bpp, alpha bits=1 |
| 16-MultiBitAlpha | 5 | 16bpp multi-bit alpha (e.g. ARGB4444) | DDPF_RGB, 16bpp, alpha bits>=2 (prefers >=5) |
| 32-NoAlpha | 6 | 32bpp opaque (e.g. XRGB8888) | DDPF_RGB, 32bpp, alpha bits=0 |
| 32-OneBitAlpha | 7 | 32bpp 1-bit alpha | DDPF_RGB, 32bpp, alpha bits=1 |
| 32-MultiBitAlpha | 8 | 32bpp multi-bit alpha (e.g. ARGB8888) | DDPF_RGB, 32bpp, alpha bits>=2 |
| Default | (first found) | Fallback | First enumerated format |

### Format Fallback Chain (in EnumerateTextureFormats)

If a preferred slot is not found, it falls back:
1. PAL8 -> first available format
2. PAL4 -> PAL8
3. 16-OneBitAlpha -> first available
4. 16-MultiBitAlpha -> 16-OneBitAlpha
5. 16-NoAlpha -> 16-OneBitAlpha
6. 32-OneBitAlpha -> 16-OneBitAlpha (or 32-MultiBitAlpha if present)
7. 32-NoAlpha -> 16-NoAlpha
8. 32-MultiBitAlpha -> 16-MultiBitAlpha

If neither 16-bit nor 32-bit multi-bit-alpha format exists, `DAT_100294b4` (FixTransparent) is set, disabling alpha-blend transparency.

### Per-Format Descriptor Record (stride 0x38 = 56 bytes)

Stored at `g_pTextureLoadDestSurface + index * 0x0E dwords`:
- +0x00: DDPIXELFORMAT (32 bytes, 8 dwords)
- +0x20: palette flag (0 or 1)
- +0x24: palette depth (4 or 8, or 0 for direct-color)
- +0x10: red bit count
- +0x14: green bit count
- +0x18: blue bit count
- +0x1C: alpha bit count

---

## Texture Management Pipeline

### DXD3DTexture Exported Methods

| VA | Export | Purpose |
|---|---|---|
| 0x100051b0 | `Environment` | Zero 0x4E8-byte texture workspace |
| 0x100051d0 | `Create` | Allocate 0x14000-byte file+aux decode buffers |
| 0x10005240 | `Destroy` | Free decode buffers |
| 0x10005270 | `Load` | Load texture from file path (.RGB, .BMP, .TGA) |
| 0x100053e0 | `LoadRGB` | Load from memory, 16-bit RGB source (type 1) |
| 0x10005480 | `LoadRGBS24` | Load from memory, 24-bit RGBS source (type 4) |
| 0x100054e0 | `LoadRGBS32` | Load from memory, 32-bit RGBS source (type 5) |
| 0x10005550 | `Manage` | Core texture build/rebuild: decode -> scratch surface -> D3D texture |
| 0x10005ba0 | `GetMask` | Return RGBA channel masks for a transparency mode (0/1/2) |
| 0x10005ea0 | `LoseAll` | Release all texture interfaces, keep metadata for RestoreAll |
| 0x10005f10 | `RestoreAll` | Rebuild all textures from stored metadata after device loss |
| 0x10006010 | `ClearAll` | Release all textures and clear bookkeeping |

### Texture Load Source Types

| Type | Source | Decoder Path |
|---|---|---|
| 0 | RGBS16 stream (decoded) | `DecodeRgbs16ToScratchSurface` |
| 1 | RGB 16-bit memory block | `UploadRgb16ToScratchSurface` (64x64 + mip chain) |
| 2 | BMP file | `DecodeBmpToScratchSurface` |
| 3 | TGA file | `DecodeTgaToScratchSurface` |
| 4 | RGBS24 memory block | `DecodeRgbs24ToScratchSurface` |
| 5 | RGBS32 memory block | `DecodeRgbs32ToScratchSurface` |

### Manage Pipeline (0x10005550)

1. **Source decode**: Based on `g_textureLoadSourceType`, decode source pixels into the scratch workspace
2. **Format selection**: `SelectTextureFormatAndLockScratchSurface` picks the best texture format slot, creates a scratch DirectDraw surface, and locks it
3. **Pixel conversion**: If needed, convert pixels to the device surface format via the converter functions
4. **Surface creation**: Create the final D3D texture surface (system memory for SW driver, video memory for HW)
5. **Texture load**: QueryInterface for IDirect3DTexture2 on both scratch and final surfaces, call `IDirect3DTexture2::Load` to transfer
6. **Registration**: Store the live texture interface in `Texture[slot * 0xC]` (stride 0x30 bytes per slot)

### Mipmap Support

- When `g_mipFilterState == 0` (mipmaps enabled), `UploadRgb16ToScratchSurface` walks the mipmap chain via `IDirectDrawSurface4::GetAttachedSurface` with caps MIPSUBMAP
- Mip levels start at 64x64 and halve each level
- Scratch surfaces are created with `DDSCAPS2_MIPMAP | DDSCAPS2_COMPLEX` flags

### Texture Slot Table

The per-texture record at `0x10029F68 + slot * 0x30` (48 bytes per slot):
- +0x00: filename (char[16]) or zero for memory-backed
- +0x10: source data pointer
- +0x14: slot index (self-reference)
- +0x18: IMAGETYPE enum
- +0x1C: dimensions
- +0x24: create flags
- Active texture interface at `Texture[slot * 0xC]`

### Device Loss / Recovery

1. `LoseAll()`: Unbinds stage 0 texture (`SetTexture(0, NULL)`), then releases every live IDirect3DTexture2 interface but preserves the metadata table
2. `RestoreAll()`: Walks the metadata table. File-backed textures are reloaded by filename via `Load`. Memory-backed textures rebuild from stored source pointer + type + flags via `Manage`

---

## Format Conversion Functions

All converters operate on a shared `tagDXIMAGELINE` descriptor that carries source/dest pointers, channel masks, dimensions, and pitch. They use a generic bit-shift approach: for each channel mask, count the leading zeros to find the MSB position, then shift source 8-bit channel values to align with the destination mask.

| VA | Function | Source Format | Output |
|---|---|---|---|
| 0x1000f530 | `ExpandPal8To16bppSurface` | 8-bit indexed | 16bpp via palette LUT |
| 0x1000f610 | `ExpandPal8To32bppSurface` | 8-bit indexed | 32bpp via palette LUT |
| 0x1000f860 | `ConvertRgb565ToSurfacePixels` | RGB565 (16-bit) | Any 16/32bpp surface |
| 0x1000fb80 | `ConvertBgr24ToSurfacePixels` | BGR888 (24-bit) | Any 16/32bpp surface |
| 0x1000fe40 | `ConvertBgra32ToSurfacePixels` | BGRA8888 (32-bit) | Any 16/32bpp surface |
| 0x10010100 | `ConvertRgb24ToSurfacePixels` | RGB888 (24-bit) | Any 16/32bpp surface |
| 0x10010290 | `ConvertArgb32ToSurfacePixels` | ARGB8888 (32-bit) | Any 16/32bpp surface |
| 0x100107e0 | `ConvertPaletteToSurfaceFormat` | Palette entries | Pre-convert to dest format |
| 0x1000f270 | `Decode4bppIndicesTo8bppSurface` | 4-bit indices | 8-bit indices |
| 0x1000f6c0 | `DecodeRlePal8To16bppSurface` | RLE pal8 | 16bpp |
| 0x10010420 | `DecodeRleRgb555ToRgb565Surface` | RLE RGB555 | RGB565 |
| 0x100105d0 | `DecodeRleBgr24ToRgb565Surface` | RLE BGR888 | RGB565 |

### Palette Alpha Handling (ConvertPaletteToSurfaceFormat)

For 1-bit alpha formats (class code 5 = 16-MultiBitAlpha in palette context):
- If the RGB value is nonzero, the alpha mask is set to `alphaMask - (alphaMask >> 1 & alphaMask)` (clears MSB of alpha = half-transparent)
- If RGB is zero (black), alpha is left at 0 (fully transparent)

For other alpha formats:
- If RGB is nonzero, alpha mask is OR'd in (fully opaque)
- If RGB is zero, alpha is left at 0

This implements **color-key transparency**: black pixels become transparent, everything else opaque.

---

## Alpha / Transparency Modes

The engine uses `DXD3DTexture::GetMask(mode, outMasks)` to query channel masks for 3 transparency levels:

| Mode | Format Slot Used | Typical Format | Use Case |
|---|---|---|---|
| 0 | 16-NoAlpha (slot 3) | RGB565 | Opaque textures |
| 1 | 16-OneBitAlpha (slot 4) | ARGB1555 | Color-keyed transparency |
| 2 | 16-MultiBitAlpha (slot 5) | ARGB4444 | Translucent textures |

**There is no stencil buffer support anywhere in M2DX.DLL.** No stencil-related render states, no stencil caps probing, no stencil format enumeration.

**Alpha blending state** (from SetRenderState):
- `ALPHABLENDENABLE` is set to 0 (disabled) in the baseline state
- `SRCBLEND = 6` (D3DBLEND_SRCALPHA) is set as the default source blend
- `DESTBLEND = 6` (D3DBLEND_SRCALPHA) -- set via the tail code when textures are supported
- The EXE is expected to toggle ALPHABLENDENABLE per-primitive at draw time

---

## Scene Management

### BeginScene (0x100015a0)

1. First-time init flag: sets `DAT_100294bc = flipChainBackBufferCount + 1` (forces N+1 clears)
2. If `bClearScreen` is set, resets the clear counter
3. If renderer not ready, tries `RestoreLostDisplaySurfaces` + `RestoreLostOverlaySurfaces`
4. Clears viewport via `IDirect3DViewport3::Clear2`:
   - First N frames: flags=3 (clear color+Z)
   - After: flags=2 (clear Z only)
   - Clear rect = (0, 0, renderWidth, renderHeight)
5. `DXDraw::CalculateFrameRate()`
6. `IDirect3DDevice3::BeginScene()`

### EndScene (0x100016b0)

1. `IDirect3DDevice3::EndScene()`
2. `DXDraw::Flip(fpsOverlayDirty)` -- present the frame

### Flip (0x10006fe0) -- Present Path

**Windowed mode**: Blt from back buffer to primary via dirty rectangle merging
- Merges two lists of dirty rects (A and B, double-buffered)
- Offsets rects by client-area origin (`DAT_10061bf4/bf8`)
- Uses `IDirectDrawSurface4::Blt` with DDBLT_WAIT flag (0x1000000)

**Fullscreen mode**: `IDirectDrawSurface4::Flip(NULL, DDFLIP_WAIT)`
- Triple-buffer dirty rect tracking swaps lists A and B each frame

---

## FullScreen Mode Switch (0x10002170)

Complete teardown-and-rebuild sequence:

1. `ResolveCompatibleD3DDriverModePair` for the requested mode index
2. Call viewport release callback (if registered)
3. If currently fullscreen, `ClearBuffers()`
4. `DXD3DTexture::LoseAll()` -- release all texture interfaces
5. Release in order: Device, ZBuffer, DAT_10031020 (unknown surface), Clipper, RenderTarget, GammaControl, Primary
6. Read resolution from display mode table: `width = g_displayModeWidthTable[mode*5]`, etc.
7. `ConfigureDirectDrawCooperativeLevel(hwnd, 1)` -- exclusive fullscreen
8. `ApplyDirectDrawDisplayMode(w, h, bpp)` -- IDirectDraw4::SetDisplayMode
9. `InitializeD3DDeviceSurfaces(driver, w, h)` -- create flip chain + Z-buffer
10. `CreateD3DDeviceForDriverRecord(driver)` -- create D3D device
11. Call viewport create callback
12. `SetRenderState()` -- apply baseline state
13. `DXD3DTexture::RestoreAll()` -- rebuild all textures

---

## Display Mode Table

The display mode table at `g_displayModeWidthTable` uses stride 5 dwords (20 bytes) per entry, max 50 entries:

| Offset | Field | Type |
|---|---|---|
| +0x00 | Width | int |
| +0x04 | Height | int |
| +0x08 | BitDepth | int |
| +0x0C | CompatibilityFlag | int |
| +0x10 | (reserved) | int |

Count in `g_displayModeCount`, current index in `g_activeDisplayModeIndex`.

---

## Z-Buffer Format Selection

`SelectZBufferFormat` (0x10003c10):
- Enumerates via `IDirect3D3::EnumZBufferFormats` for the active device GUID
- Records up to N format descriptors (32 bytes each) at `DAT_10028a9c`
- Selection strategy:
  - Default: pick shallowest depth (smallest bit count)
  - If `g_matchBitDepthState` is set: match display bit depth
  - If budget-constrained: pick shallowest to save VRAM

Z-buffer depth is stored at `DAT_10028be4` and used for VRAM budget calculations.

---

## Startup Configuration Tokens

The token table at `PTR_DAT_10027bf8` (26 entries, indices 0-25):

| Index | Token | Global Affected | Effect |
|---|---|---|---|
| 0-4 | (reserved/unknown) | | Return 0 (not consumed) |
| 5 | `Log` | `DXWin::bLogPrefered` | Enable logging |
| 6 | `TimeDemo` | `DAT_10061c40` | Demo timing mode |
| 7 | `FullScreen` | `DAT_10061c1c` | Force fullscreen |
| 8 | `Window` | `DAT_10061c1c=0` | Force windowed |
| 9-10 | `NoTripleBuffer` / `DoubleBuffer` | `g_tripleBufferState=4` | Disable triple buffering |
| 11 | `NoWBuffer` | `g_worldTransformState=4` | Disable W-buffer |
| 12 | `Primary` | `g_adapterSelectionOverride=1` | Force primary adapter |
| 13 | `DeviceSelect` | `g_adapterSelectionOverride=-1` | Show adapter picker |
| 14 | `NoMovie` | `DAT_10061c0c=4` | Skip movie playback |
| 15 | `FixTransparent` | `DAT_100294b4=1` | Force transparency fix mode |
| 16 | `FixMovie` | `DAT_10061c44=1` | Movie compatibility fix |
| 17 | `NoAGP` | `DAT_1003266c=4` | Disable AGP textures |
| 18 | `NoPrimaryTest` | `g_skipPrimaryDriverTestState=1` | Skip primary driver test |
| 19 | `NoMultiMon` | `g_ddrawEnumerateExState=4` | Disable multi-monitor enum |
| 20 | `NoMIP` | `g_mipFilterState=4` | Disable mipmapping |
| 21 | `NoLODBias` | `g_lodBiasState=4` | Disable LOD bias |
| 22 | `NoZBias` | `g_zBiasState=4` | Disable Z-bias |
| 23 | `MatchBitDepth` | `g_matchBitDepthState=1` | Match Z-buffer to display depth |
| 24 | `WeakForceFeedback` | `DXInput::FFGainScale=0.5` | Half force feedback |
| 25 | `StrongForceFeedback` | `DXInput::FFGainScale=1.0` | Full force feedback |

---

## Key Globals Summary

### D3D Core Objects

| Address | Type | Name |
|---|---|---|
| 0x10031000 | IDirect3D3* | `g_pDirect3D` |
| 0x10031004 | IDirect3DDevice3* | `g_pDirect3DDevice` |
| 0x10031008 | IDirect3DViewport3* | `g_pDirect3DViewport` |
| 0x1003100c | IDirectDrawSurface4* | `g_pZBufferSurface` |
| 0x10031010 | IDirectDrawSurface4* | `g_pPrimarySurface` |
| 0x10031014 | IDirectDrawSurface4* | `g_pRenderTargetSurface` |
| 0x10031018 | IDirectDrawClipper* | `g_pWindowClipper` |
| 0x1003101c | IDirectDrawGammaControl* | `g_pGammaControl` |

### Render State Cache

| Address | Name | Default |
|---|---|---|
| 0x10029490 | Texture clamp state (0=wrap, 1=clamp) | 0 |
| 0x10029494 | Texture support flag | 1 |
| 0x1002949c | Z-enable value | 1 |
| 0x100294b0 | Fog enabled | 0 |
| 0x100294b4 | FixTransparent flag | 0 |
| 0x100294bc | Clear-countdown (frames to clear color+Z) | flipCount+1 |

### Driver Selection

| Address | Name |
|---|---|
| 0x10028e6c | `g_d3dDriverRecordTable` (base of 5 * 0x168-byte records) |
| 0x10028a60 | Shade mode default (2=GOURAUD) |
| 0x10028a88 | Fill mode default (1=POINT, actually wireframe/solid depending on init) |

---

## Memory Management (0x10001270)

`InitializeMemoryManagement` probes VRAM via `IDirectDraw4::GetAvailableVidMem`:
1. First call with DDSCAPS_TEXTURE gets total texture memory
2. Second call with full surface caps gets free/total
3. Computes `g_surfaceMemoryBudgetBytes` as a VRAM headroom estimate
4. For shared memory: `budget = free + 0x1C2000` (1.8MB headroom)
5. Filters display modes: any mode requiring > budget for 3x front+back buffers is marked incompatible

---

## Observations for Widescreen Patching

1. **No hardcoded resolutions in any DXD3D method** -- all resolution values flow from the display mode table and `g_activeRenderWidth/Height` globals
2. **Texture format selection is fully dynamic** -- no resolution dependency
3. **SetRenderState is resolution-independent** -- all state values come from driver caps or config globals
4. **Viewport clear rect** in BeginScene uses `g_windowedRenderWidth/Height` -- automatically adapts
5. **The only resolution gate** is the 4:3 filter in `RecordDisplayModeIfUsable` (0x10008020, documented separately) which prevents non-4:3 modes from entering the display mode table
6. **Surface budget filtering** in InitializeMemoryManagement could theoretically reject high-res modes if VRAM is tight, but this is VRAM-dependent, not aspect-ratio-dependent
