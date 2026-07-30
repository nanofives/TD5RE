# D3D12 Port — Pre-flight results (§4 of D3D12_PORT_PLAN.md)

Run 2026-07-30 on branch `d3d12-port` (off `5e75086b`). Host: Windows 11,
bundled MinGW-w64 GCC 16.1.0 (`td5mod/deps/mingw64`), Windows SDK 10.0.26100.0 fxc.

## #1 — MinGW d3d12.h C-macro coverage — **GREEN**

- Bundled header family present: `d3d12.h`, `d3d12sdklayers.h`, `d3d12shader.h`,
  `d3d12video.h`; import lib `libd3d12.a` present. DRED types
  (`ID3D12DeviceRemovedExtendedDataSettings`, 205 hits) and full COBJMACROS C-macro
  wrappers (`ID3D12Device_CreateCommittedResource`, …) present.
- Smoke test (`td5mod/ddraw_wrapper/tools/d3d12_smoke.c`, ~90 LOC) compiles **and
  links** with `-c -m64 -O2 -Wall -DWIN32` + `-ld3d12 -ldxgi -ldxguid -luuid -lole32`.
  The `IID_*` GUID symbols require **`-ldxguid`** (already in `link_libs.txt`);
  **`-ld3d12` must be added** to `link_libs.txt` for the D3D12 lib.
- At runtime on this host a **real FL 11_0 device was created** (`S_OK`), plus direct
  command queue, allocator, graphics command list, fence, shader-visible descriptor
  heap, committed upload resource, serialized + created root signature (with a static
  sampler). Only the dummy `CreateSwapChainForHwnd` returned `E_ACCESSDENIED`
  (0x80070005) because `GetDesktopWindow()` isn't a valid swapchain target — that call
  was a compile/link shape check only.
- **Conclusion:** no shim needed; C is fully usable for the port. No STOP.

## #2 — fxc SM 5.0 recompile of all 21 shaders — **GREEN**

- All 21 HLSL (2× `vs_5_0`, 19× `ps_5_0`) recompile clean, zero warnings/errors, via
  the same SDK fxc used today. `ps_common.hlsli` untouched.
- Disasm diff (`/Fc`) on the hot shaders: `ps_modulate` = **65 instruction slots at
  both SM4.0 and SM5.0** (identical); `vs_pretransformed` identical target swap only.
  SM4→5 is a no-op transform as the plan predicted.
- **Note:** invoke fxc from **PowerShell**, not MSYS bash — bash mangles fxc's
  `/`-flags into POSIX paths ("Too many files specified"). Known repo gotcha.

## #3 — PIX / RenderDoc — **absent (non-blocking)**

- Neither RenderDoc nor PIX installed in default locations. **The D3D12 debug layer is
  available** (`C:\Windows\System32\d3d12SDKLayers.dll` present) — it is the primary
  correctness net, and the Phase 2 framedump A/B script is the primary render net.
- Recommend installing RenderDoc (on account3) *if* barrier/PSO debugging gets hard in
  Phase 3/4. Not a gate.

## #4 — Flip-model + composite interaction — **design note captured**

- With `FLIP_DISCARD`/`BufferCount=2` the backbuffer index rotates. The HUD color-key
  composite (`Backend_CompositeAndPresent`) writes the swapchain backbuffer, so the
  composite target must be `IDXGISwapChain3::GetCurrentBackBufferIndex()`-aware from
  Phase 1. Also affects the framedump path in `td5_platform_win32_window.c`, which
  currently grabs buffer index 0 unconditionally.

## #5 — Two-lib backend-select + stale-lib trap — **plan for Phase 0**

- Build two libs whose filename encodes the backend (`libddraw_wrapper_d3d11.a` /
  `libddraw_wrapper_d3d12.a`) so a stale worktree copy fails to *link*, not to *run*
  (the documented stale-prebuilt-wrapper crash bites twice as hard with two backends).

## Surface-area finding (divergence from plan §0)

`ID3D11` symbols leak outside the three `d3d11_backend_*.c` files far more than §0
documented (~340 refs across **10** files, not 3):

| File | ID3D11 refs | Nature |
|---|---:|---|
| `td5_platform_win32.c` | 101 | direct `ID3D11DeviceContext_*` (IA/draw/state) — §0 knew this |
| `surface4.c` | 38 | raw texture/SRV/RTV/staging in `WrapperSurface` — §0 knew this |
| `texture2.c` | 38 | raw texture/SRV in `Texture` — §0 knew this |
| `device3.c` | 26 | state cache writes |
| `ddraw_main.c` | 24 | DXGI factory + device plumbing |
| `td5_frontend.c` | 22 | **game-layer SDF-UI mini-renderer**: owns raw `ID3D11PixelShader*`/`ID3D11Buffer*` (msdf/roundrect/arrow/cursor/gauge), calls `OMSetBlendState` etc. directly on `g_backend.context` |
| `td5_platform_win32_window.c` | 22 | framedump: `IDXGISwapChain_GetBuffer` + staging Map |
| `png_loader.c` | 13 | texture upload |
| `td5_fe_menu.c` | 14 | creates/releases the SDF-UI pixel shaders directly |
| `viewport3.c` | 4 | misc |

The **game-layer SDF-UI renderer** (`td5_frontend.c` + `td5_fe_menu.c`) is not in the
plan's §0 "leakage" list and needs its own new `Backend_*` API surface in Phase 0
(create-PS-from-bytes, create/update CB, set blend, draw). This raises Phase 0's real
cost above the plan's ~600-900 LOC churn estimate.
