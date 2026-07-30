# D3D12 Port Plan — full ddraw_wrapper backend migration (D3D11 → D3D12)

Status: **PLANNED** (authored 2026-07-30, not started). Execute on a dedicated branch
(suggested: `d3d12-port`). Raster feature parity ONLY — raytracing/DXR is explicitly
out of scope (shelved; see `docs/plans/X64_DXR_ROADMAP.md` for the abandoned RT plan).
This port is what later *unblocks* DXR (Tier 12 confirmed on the RTX 5070 Ti in x64)
and the parked CSM rebuild (`csm-shadow-wip` commit message: precision rework was
deferred "to D3D12"), but neither is part of this plan.

## 0. Ground truth (surveyed 2026-07-30, post-x64-retirement tree)

What exists today (file:line refs valid at commit `e98d80ac`):

- **Wrapper = 11 C modules, 8,397 LOC** (`wrapper_srcs.txt`). The D3D11-specific core is
  **3,170 LOC in three files**: `d3d11_backend_device.c` (2069), `d3d11_backend_pipeline.c`
  (678), `d3d11_backend_draw.c` (423). The other ~5.5k LOC (ddraw_main/ddraw4/surface4/
  d3d3/device3/viewport3/texture2/png_loader) is COM-shape emulation that touches D3D11
  mostly *through* the backend — but not exclusively (see Phase 0).
- **Device**: `Backend_CreateDevice` (d3d11_backend_device.c:1737),
  `D3D11CreateDeviceAndSwapChain`, FL 11_0/10_1/10_0, `BufferCount=1`,
  `DXGI_SWAP_EFFECT_DISCARD`, `B8G8R8A8_UNORM`, own display window, DXGI Alt+Enter
  enabled, exclusive fullscreen via `SetFullscreenState`.
- **Draw path**: all T&L is CPU-side; the GPU only ever sees **pre-transformed XYZRHW
  screen-space verts**, 32-byte stride, FVF 0x1C4 (POSITION float4 / COLOR0 BGRA /
  COLOR1 BGRA (repurposed: world-normal+matid for G-buffer) / TEXCOORD0 float2).
  Batching lives in the port (`td5_render.c:675`, 1024 verts / 4096 idx per flush).
  Upload: `Backend_StreamUpload` ring — `Map(WRITE_NO_OVERWRITE)` append,
  `WRITE_DISCARD` on wrap, 16 MB VB / 4 MB IB, 16-bit indices.
- **State cache**: `RenderStateCache` (td5_wrapper_backend.h:82-123) written by
  `Dev3_SetRenderState` (device3.c:426) + port calls, resolved by
  `Backend_ApplyStateCache` (d3d11_backend_pipeline.c:169) into pre-built immutable
  objects: **8 blend × 6 depth-stencil × 2 rasterizer (default / shadow-decal
  polygon-offset) × 4 samplers (point/linear × wrap/clamp)**. Fog/alpha-test go to
  `cb_fog`. PS chosen per state (`Backend_SelectPixelShader`, pipeline.c:119),
  incl. foliage-AA variants.
- **Passes**: G-buffer-lite MRT (normal+matid, R8G8B8A8), depth as R32_TYPELESS with
  writable DSV + read-only DSV + SRV (soft particles / depth-march), screen-space
  **light / shadow / SSR** fullscreen passes (pipeline.c:312+), then
  `Backend_CompositeAndPresent` (draw.c:171): scene + color-keyed HUD overlay
  composite → Present(vsync).
- **Textures**: `UpdateSubresource` everywhere; 16bpp→BGRA CPU conversion; single
  texture unit; per-surface `device_generation` stamp + `EnsureDeviceCurrent` lazy
  rebuild after device loss; photo-booth readback via STAGING+Map.
- **Shaders**: **21** (2 VS + 19 PS), 1,546 HLSL LOC, compiled **offline by fxc to
  SM 4.0** (`compile_shaders.bat`) into committed `*_bytes.h`; no d3dcompiler link;
  CI consumes the committed headers.
- **Diagnostics**: `TD5RE_D3D_DEBUG` debug layer + InfoQueue drain; per-present frame
  forensics (256-draw ring + 16-present FRAME HISTORY + VRAM-vs-budget via
  `IDXGIAdapter3::QueryVideoMemoryInfo`) from commit 3b4739e8; `TD5RE_DXGI_TRIM`;
  `TD5RE_DRAW_EXTENT_LOG`.
- **Device-lost recovery**: `Backend_NoteDeviceRemoved` (device.c:400) →
  `Backend_RecreateDevice` (device.c:1629) → release-all + recreate +
  `device_generation++`; every submit guarded by `g_backend.device_removed`.
  Known unfixed: ~64 MB + ~100 handles leaked per recreation.
- **Build**: MinGW-w64 GCC (bundled), C not C++, static `libddraw_wrapper.a`;
  links `-ld3d11 -ldxgi` (link_libs.txt); wrapper_srcs.txt/wrapper_cflags.txt are
  single-source config shared with Makefile + CI (`_build-td5re.yml:54-75`).
- **Leakage outside the backend files** (must be fenced before the port):
  `td5_platform_win32.c` calls `ID3D11DeviceContext_*` directly
  (e.g. :3524/:3649 IASetIndexBuffer) and `surface4.c`/`texture2.c` own raw
  `ID3D11Texture2D/SRV/RTV` pointers in `WrapperSurface`.

## 1. Strategy

**Same wrapper API, new engine room.** The `Backend_*` surface (wrapper.h) and the COM
emulation layer stay; the three `d3d11_backend_*.c` files are replaced by
`d3d12_backend_*.c` equivalents. The game (`td5_platform_win32.c` and above) should not
notice, except through Phase 0's opaque-handle cleanup.

**Side-by-side during bring-up, delete at the end.** Both backends compile in-tree
behind a link-time/module-list switch plus a runtime guard knob (`TD5RE_BACKEND=11|12`,
default 12 once parity screenshots pass). This exists ONLY so every phase can A/B
framedump-compare against the known-good D3D11 output; the final phase deletes the
D3D11 files. Do not let the dual-backend state outlive the port — it is scaffolding,
not a feature.

**C, MinGW, offline shaders — unchanged.** D3D12 is fully usable from C via COBJMACROS
(`ID3D12Device_CreateCommittedResource(...)` style, same idiom the codebase already
uses for D3D11). Keep offline shader compilation and committed byte arrays: recompile
the existing HLSL with `fxc /T vs_5_0|ps_5_0` (D3D12 accepts DXBC ≤ SM 5.1; no DXC/DXIL
needed for raster parity, no new toolchain dependency). Verify first that the bundled
MinGW-w64 headers ship a usable `d3d12.h` with C macros (they vendored `d3d11on12.h`
already, so the D3D12 header family is almost certainly present in
`td5mod/deps/mingw64`). If the C macro coverage has gaps, add a small
`d3d12_compat.h` shim rather than switching languages.

**Frames in flight: 2.** Smallest number that gets pipelining; keeps the upload-ring
and deferred-deletion logic simple. All CPU-write GPU-read resources are per-frame
duplicated or fence-guarded.

## 2. Phases

Each phase ends with a verification gate. Do not start phase N+1 with N's gate red.

### Phase 0 — Fence the backend boundary (D3D11-only refactor, no D3D12 yet)

Goal: after this phase, `d3d11.h` types appear ONLY inside the three backend files.

1. Replace raw `ID3D11Texture2D/ShaderResourceView/RenderTargetView` members of
   `WrapperSurface` (surface4.c) and `Texture` (texture2.c) with an opaque
   `BackendTexture*` handle; add `Backend_TextureCreate/Upload/Destroy/GetToken`
   API. `GetHandle`'s opaque token work (commit 96e9de41) already points the way —
   extend it so no COM file dereferences a D3D type.
2. Move the direct `ID3D11DeviceContext_*` calls in `td5_platform_win32.c`
   (:3524, :3649 area) behind `Backend_*` entry points (`Backend_DrawIndexed`
   already nearly exists inside StreamUpload/draw path — formalize it).
3. Introduce `backend_iface` seam: either a function-pointer table or simple
   compile-time selection of `d3d11_backend_*.c` vs `d3d12_backend_*.c` in
   `wrapper_srcs.txt` + `TD5RE_BACKEND` runtime refusal knob. Compile-time
   selection of the module set with a tiny dispatch file is recommended
   (both backends define the same `Backend_*` symbols → keep both .c sets in the
   lib is impossible without prefixing; simplest: build two libs
   `libddraw_wrapper_d3d11.a` / `libddraw_wrapper_d3d12.a` and let
   build_standalone.bat pick via an env/arg, mirroring the existing dev/release
   dual-build pattern).
4. Regenerate nothing visual: this phase must be **bit-identical**. Gate: full
   selftest suite + golden traces + framedump byte-compare on the 3-4 standard
   scenes vs pre-phase build.

Estimated size: ~600-900 LOC churn, mostly mechanical.

### Phase 1 — D3D12 device + swapchain + clear/present skeleton

New file `d3d12_backend_device.c`.

1. `D3D12CreateDevice` (FL 11_0 min — same floor as today), `ID3D12CommandQueue`
   (direct), 2× command allocators + 1 graphics command list, fence + event,
   frame-index rotation.
2. Swapchain via `CreateSwapChainForHwnd`: **`DXGI_SWAP_EFFECT_FLIP_DISCARD`,
   `BufferCount=2`**, `B8G8R8A8_UNORM`. This is a forced change from today's
   BufferCount=1/DISCARD — harmless here because the game never renders to the
   swapchain directly (it renders to the backbuffer `WrapperSurface`; composite
   blits at present), so flip-model semantics stay invisible to game code.
   Recreate the existing display-window management (`Backend_CreateDisplayWindow`
   logic carries over verbatim). Decision to make during implementation:
   keep exclusive `SetFullscreenState` or move to borderless-fullscreen — flip
   model makes borderless cheap; keep the current exclusive behavior first for
   parity, revisit after cutover.
3. Debug layer parity: `ID3D12Debug::EnableDebugLayer` under `TD5RE_D3D_DEBUG`,
   `ID3D12InfoQueue` drained to the same `log/gpu_d3d_debug.log`. Add **DRED**
   (`ID3D12DeviceRemovedExtendedDataSettings`, auto-breadcrumbs + page-fault data)
   under the same knob — this is a strict upgrade over the hand-rolled draw ring
   for TDR forensics (keep the ring too; it is cheap and always-on).
4. RTV/DSV/CBV-SRV/sampler **descriptor heaps**: small static RTV/DSV heaps; one
   shader-visible CBV_SRV_UAV ring heap (per-frame region); samplers become
   **static samplers in the root signature** (only 4 combos exist — point/linear ×
   wrap/clamp — they all fit as static samplers, eliminating a whole heap).
5. Gate: window opens, clears to a test color, presents at vsync, clean shutdown,
   zero debug-layer errors, `TD5RE_FRAMEDUMP` captures the clear color.

### Phase 2 — Root signature, PSOs, geometry path (first triangles)

New file `d3d12_backend_pipeline.c` (state translation) + upload machinery in device file.

1. **One root signature** for the whole raster pipeline: root CBV b0 (frame/fog CB),
   root CBV b1 (pass CB), descriptor table t0 (1 SRV — single texture unit!), 4
   static samplers s0-s3 (sampler *choice* moves from bind-time to... note: today
   the sampler is a dynamic state — fold it into the PSO key or index static
   samplers from a root constant; **root constant selecting s0-s3 in the shader is
   the smallest-diff option** since the 21 shaders otherwise need per-sampler
   variants).
2. **PSO cache** keyed by the same fields `RenderStateCache` already tracks:
   `{VS, PS, blend_idx(8), ds_idx(6), raster_idx(2), topology(tri/line)}`.
   Upper bound is small (≤ a few hundred reachable combos; in practice a few
   dozen). Hash-map cache built on demand + optional warm-up list of the known-hot
   combos at device create. `Backend_ApplyStateCache` becomes "resolve cache →
   PSO lookup → `SetPipelineState` + root CBV updates" — the *callers don't change*.
3. **Upload ring** replacing Map/NO_OVERWRITE: one persistent-mapped upload-heap
   buffer per frame-in-flight (16 MB VB + 4 MB IB regions, same sizes), append
   offset reset at frame start after fence wait. Pre-transformed verts are
   write-once-read-once, so drawing straight from the upload heap is correct and
   fastest here — no default-heap copy step (CPU-write + GPU-read-once is exactly
   what upload heaps are for).
4. Port `vs_pretransformed` + the modulate/decal PS family first (recompile all 21
   shaders to SM 5.0 in `compile_shaders.bat`; keep the `_bytes.h` scheme, add the
   new arrays alongside until D3D11 is deleted).
5. Constant buffers: the 5 CBs move to per-frame upload-ring slices (root CBVs
   point at ring offsets — no descriptor churn).
6. Gate: frontend renders correctly (menus are the simplest full exercise of
   draw+state+texture path); framedump compare vs D3D11 within tolerance
   (expect bit-identical for opaque UI; alpha ordering identical since draw order
   is preserved).

### Phase 3 — Textures, surfaces, HUD overlay

1. `BackendTexture` D3D12 implementation: committed default-heap resources; upload
   via per-frame upload-heap staging + `CopyTextureRegion` on the frame's command
   list (replaces `UpdateSubresource`). Keep the existing CPU-side `sys_buffer` +
   `FlushDirty` dirty-rect scheme in surface4.c — only the final upload call
   changes, the 16bpp→BGRA conversion and dirty tracking carry over untouched.
2. **Deferred deletion queue**: any resource released while potentially referenced
   by an in-flight frame goes on a `(fence_value, resource)` list, drained after
   fence completion. This replaces D3D11's immediate-Release semantics and is the
   single biggest new correctness obligation of the port — route ALL releases
   through it from day one (including PSOs on cache eviction, though eviction
   likely never happens).
3. Resource-state tracking: per-resource last-state field + a tiny
   `Backend_Transition(res, new_state)` helper that batches barriers. The pass
   graph is fixed and shallow (scene RT ↔ SRV, depth DSV ↔ SRV, G-buffer RT ↔ SRV,
   scene-copy COPY_DEST ↔ SRV) — a full automatic barrier system is overkill;
   hand-placed transitions at pass boundaries + the helper for surfaces suffice.
4. Photo-booth/framedump readback: readback-heap buffer + `CopyTextureRegion` +
   fence wait (replaces STAGING Map). `TD5RE_FRAMEDUMP` must work from this phase
   on — it is the verification instrument for everything else.
5. Composite/present path (`d3d12_backend_draw.c`): scene RT + color-keyed HUD
   overlay composite (same `ps_composite`), barrier to PRESENT, Present(vsync).
   `TD5RE_DXGI_TRIM` parity: `IDXGIDevice3::Trim` doesn't apply to a D3D12 device —
   the equivalent is `ID3D12Device::Evict`/budget management; simplest parity is
   wiring the knob to a no-op + log line first, and only implementing eviction if
   the memory numbers regress (the in-place-texture fix already killed the
   pressure this knob was added for).
6. Gate: full frontend + in-race render correct on 2 tracks; framedump A/B diff;
   VRAM/private-bytes within a few % of the D3D11 numbers over a 3-race soak
   (`--SelfTest=1` smoke).

### Phase 4 — Offscreen passes: depth-read, G-buffer, light/shadow/SSR

1. Depth: same R32_TYPELESS texture; D3D12 also supports read-only DSV
   (`D3D12_DSV_FLAG_READ_ONLY_DEPTH`) + simultaneous SRV — port the soft-particle /
   depth-march binding pattern directly, with explicit DEPTH_WRITE ↔
   DEPTH_READ|PIXEL_SHADER_RESOURCE transitions.
2. G-buffer MRT: second RTV on the pass PSOs (`ps_modulate_g` family); PSO key
   grows one bit (`gbuffer_on`) — the D3D11 code toggles this by rebinding RTs,
   the D3D12 version needs it in the PSO's NumRenderTargets/RTVFormats.
3. Screen-space light/shadow/SSR passes + fullscreen quad (`vs_fullscreen`
   SV_VertexID path ports as-is). SSR's scene copy: `CopyResource` →
   `CopyTextureRegion` with COPY_SOURCE/COPY_DEST barriers.
4. Foliage-AA shader selection: pure PS choice, carries through the PSO key
   unchanged.
5. Gate: night race + rain + foliage-heavy track framedumps A/B vs D3D11;
   the light/shadow/SSR knobs behave identically; `TD5RE_DRAW_EXTENT_LOG` numbers
   comparable.

### Phase 5 — Device-lost recovery + forensics parity

1. Detect removal: `Present`/`ExecuteCommandLists` HRESULTs + 
   `ID3D12Device_GetDeviceRemovedReason`; same latch (`Backend_NoteDeviceRemoved`)
   and same `log/gpu_device_lost.log` format, now enriched with the DRED
   breadcrumb + page-fault dump when the debug knob is on.
2. `Backend_RecreateDevice` D3D12 edition: release queue/allocators/lists/fence/
   heaps/PSO cache/upload rings/swapchain/device, recreate, `device_generation++` —
   the lazy `WrapperSurface`/`BackendTexture` rebuild machinery from Phase 0/3 is
   backend-agnostic and carries over. **Fix the known ~64 MB + ~100 handle leak
   per recreation while rebuilding this path** (it's currently unticketed; the
   deferred-deletion queue makes the fix natural — everything is on one list).
3. Frame forensics: `Backend_NoteDraw/NoteVerts/NotePresent` ring + FRAME HISTORY
   are API-agnostic CPU code — recompile as-is; VRAM query via the same
   `IDXGIAdapter3::QueryVideoMemoryInfo`.
4. Gate: induced TDR (the race-moscow rapid-fire harness from the 2026-07-30
   forensics session) recovers to a playable state with no debug-layer errors and
   **no leak growth across 5 recreations** (this is a behavior *improvement* gate,
   not just parity).

### Phase 6 — Soak, cutover, deletion

1. Full verification battery (see §3) green on D3D12.
2. Flip the default: `build_standalone.bat` links the D3D12 lib; D3D11 buildable
   only via explicit arg for one grace commit.
3. Delete `d3d11_backend_*.c`, the D3D11 `_bytes.h` arrays (if SM5.0 replaced
   them), the backend-select scaffolding, and `-ld3d11` from link_libs.txt
   (keep `-ldxgi`; add `-ld3d12`). Update wrapper_srcs.txt, Makefile, CI workflow
   env, `docs/ARCHITECTURE_RENDER_FLOW.md` (§3/§5 rewritten), CLAUDE.md's wrapper
   references ("D3D11 backend" → "D3D12 backend"), EXPECTED_BEHAVIOR.md:127.
4. Re-record the structure-lint warning baseline if counts shift; regenerate the
   module table if module names changed.
5. Gate: CI green, full selftest suite + goldens, release build boots and races.

## 3. Verification (applies throughout)

- **Golden traces are the sim net and MUST stay untouched** — this port must not
  change a single sim tick. Any golden mismatch during this work is a bug in the
  port scaffolding (e.g. timing-dependent input), full stop.
- **Framedump A/B** is the render net: fixed-seed RaceTrace configs + 
  `TD5RE_FRAMEDUMP` at pinned ticks, D3D11 vs D3D12, per phase. Expect
  bit-identical for opaque geometry (same DXBC-family shaders, same vert data);
  allow small tolerance only where blending order interacts with precision.
  Build the A/B script once in Phase 2 (`scripts/` + control-socket screenshot
  verb) and reuse it every phase.
- **Selftest suite** (`pwsh scripts/selftest.ps1 -Suite full`) at every phase gate
  from Phase 3 on; degradation monitors (private bytes / GDI / handles / frame
  times) are the leak net for the new deferred-deletion machinery.
- **Debug layer clean**: `TD5RE_D3D_DEBUG=1` run must produce zero errors/corruption
  warnings at every gate (D3D12's validation is far stricter than D3D11's — treat
  its warnings as errors during bring-up; they are tomorrow's TDR).
- **Perf**: frame-time capture (`td5_benchmark.c`) on the heaviest scene
  (race-moscow, 6 racers + traffic, night+rain) — D3D12 must be ≥ D3D11. If it is
  not, the likely culprits are barrier spam or PSO misses mid-frame (log PSO-cache
  misses after warm-up; steady state should be zero).

## 4. Risks & pre-flight checks (do these FIRST, before Phase 0)

1. **MinGW d3d12.h C-macro coverage** — compile a 30-line C smoke test
   (create device, queue, swapchain, fence) against the bundled toolchain.
   If broken: mingw-w64 headers are updatable in-place, or shim. This is the only
   external unknown in the whole plan; everything else is in-tree.
2. **fxc SM5.0 recompile** of all 21 shaders — do it up front, diff the
   disassembly for surprises (SM4→5 is routine; ps_common.hlsli untouched).
3. **PIX / RenderDoc availability** for D3D12 on this machine — you will want a
   frame debugger for barrier/PSO bugs; verify capture works on the Phase 1
   skeleton before you need it in anger.
4. **Flip-model + composite interaction**: the HUD color-key composite reads the
   scene RT and writes the swapchain backbuffer — with flip model the backbuffer
   index rotates; make the composite target `GetCurrentBackBufferIndex()`-aware
   from the start (classic flip-model porting bug).
5. **Two sessions / worktrees**: the wrapper static lib is copied into worktrees —
   the stale-`libddraw_wrapper.a` crash trap is documented
   (memory: worktree-stale-prebuilt-wrapper) and will bite twice as hard with two
   libs; make the backend-select arg part of the lib filename so a stale copy
   fails to *link*, not to *run*.

## 5. Size & sequencing estimate

| Phase | New/changed LOC (est.) | Depends on |
|---|---|---|
| 0 — fence boundary | ~600-900 churn | — |
| 1 — device skeleton | ~800 new | 0 |
| 2 — PSO + geometry | ~900 new | 1 |
| 3 — textures/surfaces/present | ~800 new + surface4 churn | 2 |
| 4 — passes | ~500 new | 3 |
| 5 — recovery/forensics | ~400 new + reuse | 3 |
| 6 — cutover/deletion | net-negative | all |

Total new D3D12 backend ≈ 3.5-4.5k LOC replacing the 3,170-LOC D3D11 core — the same
order of magnitude, as expected for raster parity. Phases 4 and 5 can proceed in
parallel sessions if isolated per the worktree rules. Every phase lands as its own
commit(s) on `d3d12-port`; the branch merges only after Phase 6's gate.
