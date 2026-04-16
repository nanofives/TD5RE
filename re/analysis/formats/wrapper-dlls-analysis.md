# Wrapper DLLs Analysis: ddraw.dll and winmm.dll

## ddraw.dll - Custom DirectDraw-to-D3D11 Translation Layer

### Summary

The ddraw.dll in the project root is a **custom-built** wrapper DLL (compiled with MinGW GCC) that
completely replaces the DirectDraw/Direct3D 6 API with a Direct3D 11 backend. It is **not** a
generic DDrawCompat-style compatibility shim -- it is a purpose-built translation layer specifically
for Test Drive 5's rendering pipeline through M2DX.dll.

### Binary Properties

- **Format**: 32-bit PE DLL (x86:LE:32), compiled with MinGW GCC
- **Image base**: 0x67500000
- **Sections**: 19 PE sections (includes `.debug_*` sections from GCC's DWARF debug info)
- **Code size**: ~125 KB in `.text`, ~1 MB BSS (global backend state)
- **Has debug symbols**: Yes (wrapper_log, Backend_Init, etc. visible as named functions)
- **Build system**: `build.bat` using MinGW GCC with `-O2 -DWRAPPER_DEBUG`

### Export Surface

Exports only 4 DirectDraw creation functions (defined in `ddraw.def`):

| Ordinal | Export                | Purpose                          |
|---------|----------------------|----------------------------------|
| @1      | DirectDrawCreate     | Main DD creation entry point     |
| @2      | DirectDrawCreateEx   | Extended DD creation (delegates to DDCreate) |
| @3      | DirectDrawEnumerateA | Enumerates "D3D11 Wrapper" as the only device |
| @4      | DirectDrawEnumerateExA | Same, extended version         |

Plus internal GCC runtime exports (`__dyn_tls_init`, `__emutls_*`, `__divdi3`, etc.).

### Import Surface -- D3D11 Backend

Key imports that reveal the translation target:

- **D3D11.DLL**: `D3D11CreateDeviceAndSwapChain` -- the core D3D11 device creation
- **DXGI.DLL**: `CreateDXGIFactory1` -- DXGI factory for display mode enumeration
- **GDI32.DLL**: `CreateDIBSection`, `CreateCompatibleDC`, etc. -- GDI for HDC surface ops
- **KERNEL32.DLL**: `GetPrivateProfileIntA` -- reads config from `scripts/td5_mod.ini`,
  `VirtualProtect` -- runtime patching of M2DX.dll and TD5_d3d.exe

### Architecture: Call Chain

```
TD5_d3d.exe  ->  M2DX.dll  ->  ddraw.dll (our wrapper)  ->  D3D11 + DXGI  ->  GPU
                                     |
                                     v
                              IDirectDraw4 vtable (ddraw4.c)
                              IDirectDrawSurface4 vtable (surface4.c)
                              IDirect3D3 vtable (d3d3.c)
                              IDirect3DDevice3 vtable (device3.c)
                              IDirect3DViewport3 vtable (viewport3.c)
                              IDirect3DTexture2 vtable (texture2.c)
```

The game calls `DirectDrawCreate()`, which initializes the D3D11 backend and returns
a `WrapperDirectDraw` COM object with a custom vtable. M2DX.dll then calls
`QueryInterface` for `IDirectDraw4`, creates surfaces, obtains `IDirect3D3`, creates a
`IDirect3DDevice3`, and renders via `DrawPrimitive`/`DrawIndexedPrimitive`.

### D3D11 Device Creation

Occurs in `Backend_CreateDevice()` (`d3d11_backend.c`):

1. Creates a dedicated "TD5_D3D11_Display" window (windowed mode) separate from M2DX's hidden window
2. Calls `D3D11CreateDeviceAndSwapChain` with a DXGI swap chain
3. Creates render target view from swap chain back buffer
4. Creates depth buffer (D3D11 Texture2D + DepthStencilView)
5. Pre-creates all immutable state objects:
   - 6 blend states (opaque, alpha, additive, etc.)
   - 6 depth-stencil states (various Z-enable/write/func combos)
   - 4 sampler states (point/linear x wrap/clamp)
   - 1 rasterizer state (CullNone, solid)
6. Creates dynamic vertex buffer (128 KB) and index buffer (32 KB) for immediate-mode rendering
7. Loads pre-compiled HLSL shaders from bytecode headers

### Resolution Scaling and Native Resolution Support

The wrapper applies **runtime binary patches** to both M2DX.dll and TD5_d3d.exe on first
`DirectDrawCreate` call:

- **M2DX.dll patches**: Bypass 4:3 aspect ratio filter (JZ->JMP), override default
  resolution to native monitor resolution
- **TD5_d3d.exe patches**: Override WinMain resolution globals, frontend canvas dimensions
  (InitializeFrontendResourcesAndState, RunFrontendDisplayLoop)
- **Frontend scaling**: `FrontendScale` struct manages transparent upscaling of the 640x480
  game UI to native resolution. BltFast coordinates are scaled, Lock/Unlock provides a
  virtual 640x480 buffer while maintaining native-res backing storage
- **Vertex coordinate scaling**: `vertex_scale_x/y` factors applied to every XYZRHW vertex
  to map M2DX's internal resolution to the native render target

### Surface Management

`WrapperSurface` replaces `IDirectDrawSurface4` with D3D11-backed storage:

- **Primary surface**: Maps to swap chain back buffer
- **Back buffer**: D3D11 render target (Texture2D + RTV + SRV)
- **Z-buffer**: D3D11 depth texture (DepthStencilView)
- **Offscreen surfaces**: System memory buffer with optional D3D11 staging texture
- **Lock/Unlock**: Maps to system memory buffer; on Unlock, data is uploaded to GPU via staging texture
- **BltFast**: Composited at present time via a fullscreen quad + composite shader

### Present Chain

At Flip/Blt-to-primary time:

1. If BltFast content exists (2D UI layer), upload to `composite_tex` via staging texture
2. Bind swap chain back buffer as render target
3. Draw fullscreen quad with backbuffer (3D scene) as texture
4. If composite content exists, overlay it with alpha blending
5. `IDXGISwapChain::Present()`

### Shader Pipeline

All rendering uses HLSL shaders (D3D11 has no fixed-function pipeline):

| Shader | File | Purpose |
|--------|------|---------|
| `vs_pretransformed` | vs_pretransformed.hlsl | XYZRHW pixel coords -> NDC clip space |
| `vs_fullscreen` | vs_fullscreen.hlsl | Fullscreen quad for present/composite |
| `ps_modulate` | ps_modulate.hlsl | `tex * diffuse`, alpha from texture (most common) |
| `ps_modulate_alpha` | ps_modulate_alpha.hlsl | `tex * diffuse`, alpha = tex * diffuse |
| `ps_decal` | ps_decal.hlsl | Texture-only (no vertex color) |
| `ps_luminance_alpha` | ps_luminance_alpha.hlsl | Luminance-alpha mode for R5G6B5 textures |
| `ps_composite` | ps_composite.hlsl | Simple texture passthrough for present blit |

All pixel shaders share `ps_common.hlsli` which provides:
- Linear fog computation (fog start/end/color from constant buffer)
- Alpha test with discard (replaces D3D6 fixed-function alpha test)

### PNG Texture Override System

`png_loader.c` implements runtime texture replacement:
- **Dump mode**: When `td5_png/dump_textures.flag` exists, dumps all game textures as PNG
- **Replace mode**: Loads CRC32-indexed PNG files to replace original R5G6B5 textures with
  authored 8-bit alpha (for proper transparency)
- Uses stb_image / stb_image_write for PNG I/O

---

## winmm.dll - Ultimate ASI Loader (ThirteenAG)

### Summary

The winmm.dll is **NOT** a simple winmm proxy. It is
**ThirteenAG's Ultimate ASI Loader** (Win32 variant), masquerading as winmm.dll.
This is a well-known modding tool that loads `.asi` plugin files into game processes.

### Binary Properties

- **Format**: 32-bit PE DLL (x86:LE:32)
- **Image base**: 0x10000000
- **Total exports**: 1,229 (!)
- **Identified by strings**:
  - `"Ultimate ASI Loader"`, `"Ultimate-ASI-Loader-Win32"`, `"Ultimate-ASI-Loader-Win32.dll"`
  - `"IsUltimateASILoader"` (exported function)
  - `"InitializeASI"` (ASI plugin entry point name)
  - `"*.asi"` (glob pattern for finding plugins)
  - `"modloader\\modloader.asi"` (modloader support)

### What It Proxies

The massive 1,229 export list reveals this is a **universal proxy DLL** that re-exports
functions from many system DLLs simultaneously:

| API Family | Examples | Purpose |
|------------|----------|---------|
| **winmm.dll** | `CloseDriver`, `DefDriverProc`, `DriverCallback`, `DrawDib*` | Multimedia / waveform audio |
| **DirectDraw** | `DirectDrawCreate`, `DirectDrawEnumerateA`, `AcquireDDThreadLock`, `DD*`, `D3DParseUnknownCommand` | DirectDraw surface internals |
| **Direct3D 8-12** | `Direct3DCreate8`, `Direct3DCreate9`, `D3D10*`, `D3D11*`, `D3D12*`, `D3DKMT*`, `D3DPERF_*` | All Direct3D versions |
| **DirectInput** | `DirectInput8Create`, `DirectInputCreateA/W/Ex` | Input system |
| **DirectSound** | `DirectSoundCreate`, `DirectSoundCreate8`, `DirectSoundEnumerate*` | Audio |
| **WinInet/URL Cache** | `InternetOpen*`, `HttpSendRequest*`, `CreateUrlCacheEntry*`, `FtpGetFile*` | Internet APIs |
| **XLive** | `XLiveInitialize`, `XLiveInput`, `XLiveGetGuideKey`, etc. (51 XLive functions) | Games for Windows Live emulation |
| **XNet/XSocket** | `XNetStartup`, `XNetCleanup`, `XCreateSocket`, `XSocket*`, `XNetXnAddrToInAddr` | Xbox network emulation |
| **AppCache** | `AppCacheCheckManifest`, `AppCacheLookup`, etc. | IE cache management |
| **Virtual File System** | `AddVirtualFileForOverloadA/W`, `AddVirtualPathForOverloadA/W`, `GetOverloadPathA/W` | File overloading |

It references both `"winmm.dll"` and `"winmmHooked.dll"` strings, confirming it loads the
real winmm.dll from System32 for proxying while intercepting the loading process.

### Import Surface

Key imports reveal its hooking mechanism:
- `VirtualAlloc`, `VirtualProtect`, `VirtualFree` -- memory patching
- `FlushInstructionCache` -- code patching
- `CreateToolhelp32Snapshot`, `Thread32First/Next` -- thread enumeration for safe hooking
- `SuspendThread/ResumeThread`, `GetThreadContext/SetThreadContext` -- thread context manipulation
- `CreateFileA/W`, `FindFirstFileA/W`, `GetFileAttributesA/W` -- file system hooks (virtual file system)
- `LoadLibraryA/W`, `GetProcAddress`, `FreeLibrary` -- dynamic library loading

### ASI Loading Mechanism

1. Game starts, Windows loads winmm.dll from game directory (DLL search order hijacking)
2. Ultimate ASI Loader initializes, loads real `winmm.dll` from System32 for proxying
3. Scans for `*.asi` files in the game directory and scripts folder
4. For each `.asi` file, calls `LoadLibrary` then looks for `InitializeASI` export
5. Hooks file I/O APIs (CreateFileA/W, FindFirstFile, GetFileAttributes) for virtual file system
6. Provides file overloading via `AddVirtualFileForOverloadA/W` and `AddVirtualPathForOverloadA/W`

### XLive and XNet Emulation

The DLL includes a substantial Xbox Live / GFWL emulation layer:
- 51+ XLive functions (XLiveInitialize, XLiveInput, XLiveGetGuideKey, etc.)
- Full XNet networking stack (XNetStartup, XNetRegisterKey, XNetXnAddrToInAddr, etc.)
- XSocket wrappers mapping Xbox socket APIs to Winsock

This suggests Test Drive 5 may use or have been patched to use Games for Windows Live
networking, or this is a universal loader that includes these for compatibility with
other games.

---

## How They Chain Together at Runtime

### Boot Sequence

```
Windows loads TD5_d3d.exe
  |
  +-> Windows resolves import "winmm.dll" -> loads winmm.dll from game directory
  |     (Ultimate ASI Loader)
  |     |
  |     +-> Loads real winmm.dll from System32 (proxy target)
  |     +-> Scans for *.asi files -> loads td5_mod.asi (and others)
  |     +-> Hooks file I/O for virtual file overloading
  |
  +-> M2DX.dll initializes (graphics middleware)
  |     |
  |     +-> Calls DirectDrawCreate() -> resolves to ddraw.dll in game directory
  |           (our D3D11 wrapper, NOT system ddraw.dll)
  |           |
  |           +-> Backend_Init(): patches M2DX.dll and EXE for native resolution
  |           +-> Creates D3D11 device + swap chain
  |           +-> Returns WrapperDirectDraw COM object
  |
  +-> Game renders via M2DX -> IDirectDraw4/IDirect3DDevice3 COM calls
  |     -> Our wrapper translates to D3D11 in real-time
  |
  +-> td5_mod.asi (loaded by winmm.dll) provides additional game patches
```

### Key Interactions

1. **winmm.dll** loads first (import resolution), establishes the ASI loader framework
2. **td5_mod.asi** is loaded by the ASI loader and applies game-level patches
3. **ddraw.dll** loads when M2DX.dll first calls DirectDrawCreate (late binding)
4. ddraw.dll reads `scripts/td5_mod.ini` for resolution settings (written by td5_mod.asi)
5. ddraw.dll patches M2DX.dll in-memory to accept widescreen resolutions
6. Both DLLs use DLL search order hijacking -- placed in game directory to intercept
   system DLL loading

### Division of Responsibilities

| Component | Role |
|-----------|------|
| **winmm.dll** (ASI Loader) | Boot-time injection framework, ASI plugin loading, file overloading, XLive emulation |
| **td5_mod.asi** | Game logic patches, configuration, mod features |
| **ddraw.dll** (D3D11 Wrapper) | Complete DirectDraw/D3D6-to-D3D11 rendering translation, resolution scaling, texture override |

---

## DDraw Wrapper Source Architecture

The DDraw-to-D3D11 translation layer. The source tree at `td5mod/ddraw_wrapper/src/`:

### Source Files

| File | Lines | Purpose |
|------|-------|---------|
| `wrapper.h` | 734 | Master header: all structures, constants, COM type definitions. Defines D3D11Backend global state, WrapperSurface, FrontendScale, RenderStateCache, D3D6 constants |
| `ddraw_main.c` | 550 | DLL entry point, DirectDraw exports, Backend_Init (with M2DX/EXE binary patches), Backend_Shutdown, GUID definitions, crash handler |
| `ddraw4.c` | ~400 | IDirectDraw4 vtable: 27 COM methods (CreateSurface, SetCooperativeLevel, SetDisplayMode, EnumDisplayModes, etc.) |
| `surface4.c` | ~800+ | IDirectDrawSurface4 vtable: 44 COM methods (Lock, Unlock, Blt, BltFast, Flip, GetSurfaceDesc, etc.). Present path via Backend_CompositeAndPresent |
| `d3d3.c` | ~300 | IDirect3D3 vtable: device enumeration, Z-buffer format enum, viewport/device creation |
| `device3.c` | ~600+ | IDirect3DDevice3 vtable: DrawPrimitive, DrawIndexedPrimitive, SetRenderState, SetTexture, BeginScene/EndScene. Core rendering translation |
| `viewport3.c` | ~150 | IDirect3DViewport3 vtable: viewport parameter management |
| `texture2.c` | ~300 | IDirect3DTexture2 vtable: texture load (surface -> SRV), GetHandle, PNG override integration |
| `d3d11_backend.c` | 800+ | D3D11 device lifecycle, display window management, shader loading, state object creation, render state cache binding, fullscreen quad, constant buffer updates |
| `png_loader.c` | ~500 | PNG texture dump/replace system using stb_image |

### Shader Files

Pre-compiled with `fxc.exe` via `compile_shaders.bat`, bytecode embedded in `*_bytes.h` headers:

- 2 vertex shaders: `vs_pretransformed` (XYZRHW -> NDC), `vs_fullscreen` (fullscreen quad)
- 5 pixel shaders: `ps_modulate`, `ps_modulate_alpha`, `ps_decal`, `ps_luminance_alpha`, `ps_composite`
- Shared include: `ps_common.hlsli` (fog + alpha test)

### Build System

`build.bat` compiles with MinGW GCC, links against `-ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -ldxguid`.
Output DLL is deployed to project root. Uses `ddraw.def` to export only the 4 DirectDraw
creation functions at fixed ordinals.

### Design Principles

1. **Pure C, no C++**: All COM objects are hand-rolled vtables (struct of function pointers)
2. **Single-threaded**: Game is single-threaded, so no synchronization needed
3. **Immediate translation**: No command buffering; each D3D6 call maps directly to D3D11
4. **State caching**: D3D6 SetRenderState calls update a cache; D3D11 state objects are
   selected and bound only when the cache is dirty
5. **Compositing**: 2D (BltFast) and 3D (DrawPrimitive) content are merged at present time
   using a fullscreen quad overlay pass
