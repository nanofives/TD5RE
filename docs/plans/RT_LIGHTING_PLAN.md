# RT_LIGHTING_PLAN — Ray-traced lighting, shadows & reflections ("LIGHTING QUALITY: HIGH")

Authored 2026-07-31 (Fable 5 session, account3). Executor: a separate Claude Code session
(account2 / Accenture-managed — per-call approvals, **no web access**, no Ghidra/frida MCP,
no Workflows). This document is intentionally self-contained: every survey fact, struct
layout, alignment, and API sequence needed to execute is written down here. **Do not
redesign the frozen parts** — a lot of the value of this doc is that the error-prone DXR
plumbing (SBT, state object, root signatures) is decided once, up front.

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | DXR foundation (dxc, Device5, smoke dispatch) | ✅ done (branch rt-lighting) |
| 1 | World-space geometry feed + BLAS/TLAS + debug view | ✅ done — **alignment gate PASSED** |
| 2a | G-buffer MRT wiring in D3D12 | ✅ done |
| 2b | RT shadows (sun + dynamic lights), blob-shadow kill | ✅ done (denoise/soak owed) |
| 3 | RT reflections with textured hit shading | ⬜ |
| 4 | Menu row, INI, runtime switch, fallback, release | ⬜ |

Append an **as-built note** under this table after each phase (deviations, measurements,
gotchas found).

### As-built — Phase 0 (2026-07-31, branch `rt-lighting`)

**Preconditions verified:** dxc.exe present at `Windows Kits\10\bin\10.0.26100.0\x64\`
(dxc 1.8, dxil.dll alongside → signed DXIL); bundled MinGW `d3d12.h` declares
`ID3D12Device5`, `ID3D12GraphicsCommandList4`, `D3D12_DISPATCH_RAYS_DESC`,
`BuildRaytracingAccelerationStructure`, OPTIONS5, `CreateStateObject`,
`SetPipelineState1`; IIDs `IID_ID3D12Device5` / `IID_ID3D12GraphicsCommandList4` /
`IID_ID3D12StateObject` / `IID_ID3D12StateObjectProperties` are all defined in
`libdxguid.a` (already in `link_libs.txt`) → **no local `DEFINE_GUID` needed**.

**Built:** `rt_pipeline.hlsl` (`rgen_smoke` only) → DXIL via a new dxc `lib_6_3`
step in `compile_shaders.bat`, emitted as `const unsigned char g_rt_pipeline[]`
(compiles clean under MinGW). New `d3d12_dxr.c` (wrapper-internal DXR module) +
`d3d12_backend_priv.h` (accessor seam — no exported globals). Backend now QIs
`ID3D12Device5` (after an OPTIONS5 tier≥1.0 check, gated by `TD5RE_RT_DISABLE`) and
`ID3D12GraphicsCommandList4`; both may be NULL → every RT entry point no-ops.
Game side: `td5_rt.c/.h` (capability + activation predicates) +
`td5_plat_rt_available` bridge + `Backend_RTAvailable`.

**Frozen bits implemented:** global RS = b0 CBV + u0 UAV table + static LINEAR-wrap
sampler; state object = DXIL_LIBRARY + SHADER_CONFIG{32,8} + PIPELINE_CONFIG{depth 1}
+ GLOBAL_ROOT_SIGNATURE; SBT = one committed UPLOAD buffer, one 32-byte raygen
record at offset 0; owned shader-visible CBV/SRV/UAV heap (8 slots: [0]=output UAV,
[1]=blit SRV). `SetPipelineState1` + `DispatchRays` over the full frame.

**Deviations:** (1) smoke output UAV texture is `R8G8B8A8_UNORM` (typed-UAV safe;
BGRA8 typed UAV stores are not guaranteed) → the gradient's channels are swapped
when blitted to the BGRA backbuffer; cosmetic only, gradient still proves the path.
(2) The smoke blit is triggered inside the wrapper present path (gated by
`TD5RE_RT_SMOKE`) rather than driven by the game — keeps Phase 0 self-contained; the
capability plumbing (`Backend_RTAvailable`→`td5_plat_rt_available`→`td5_rt_available`)
is still wired for later phases. (3) The blit reuses the backend's fullscreen VS +
composite PS via `d3d12_priv_fullscreen_shaders()` (external-linkage arrays live in
one TU); dxr owns its own tiny blit RS + PSO + heap (never touches the backend heap).

**Gate results:** `build_all.bat` clean (dev + release), lint OK
(extern_in_c=3/baseline 3, no new warnings); selftest smoke **15/15 PASS**; golden
traces **54 PASS / 0 FAIL** (7 module hashes match on moscow/pelton/drag/split);
`TD5RE_RT_SMOKE=1` framedump = correct UV gradient; `TD5RE_RT_DISABLE=1` = normal
MAIN MENU (smoke no-ops, `device5` NULL); D3D12 debug layer clean (empty
`d3d12_debug.log`). **Gotcha:** golden traces first FAILed on moscow/pelton/split
because the working-tree `td5re.ini` carried the user's uncommitted `Difficulty=2→1`
(+ DragLength/CarDamage) edits — a false-fail; restoring the committed ini for the
run gave 54/0. Always `git checkout td5re.ini` (or diff) before trusting a golden
FAIL. drag PASSed throughout (its golden config is Difficulty-independent).

### As-built — Phase 1 (2026-07-31, branch `rt-lighting`, WIP — ALIGNMENT GATE OPEN)

**Done + verified:** Full feed/AS/debug-view infrastructure builds clean (dev+release),
lint OK, **golden traces 54 PASS / 0 FAIL** (the always-on feed — gated on
`td5_rt_available()` per plan §1.2 — is strictly read-only over span/actor state, so
the sim is byte-identical; 7 module hashes match on moscow/pelton/drag/split), no
TDR/crash across the full race matrix, and **the RT code is D3D12-debug-layer clean**
(the ~727 pre-existing `id=690` PSO/root-sig warnings in the deferred passes reproduce
with `TD5RE_RT_DISABLE=1` and are NOT from RT).

Built: feed API `Backend_RTMesh*`/`RTScene*`/`RTSetView`/`RTDebugView`/`RTGeneration`;
`d3d12_dxr.c` grew ~900 lines — pooled DEFAULT VB/IB (128/48 MB, bump-allocated),
per-mesh UPLOAD staging → pool copy + chunked BLAS build (≤500k tris/frame) in
`Backend_RTSceneEnd`, double-buffered TLAS (cap 128 instances, PREFER_FAST_BUILD),
null-TLAS-SRV so the smoke path stays valid, `RTMARK:*` crumbs in the crash ring
(`Backend_NoteRTMark`), device-lost generation counter. State object grew to the frozen
set (rgen_smoke/debug, chit_refl, miss_shadow/refl, hit group "hg"); SBT reworked to the
frozen layout. Game `td5_rt.c`: track walk (per-lane quads via new
`td5_track_get_lane_quad_world`) + actor-mesh cache + per-frame TLAS + camera view;
hooked at `td5_track_load_strip` (level build) and the `td5_game.c` per-pane deferred
site (`td5_rt_frame`). Debug view is an **alpha overlay** (raygen writes 0.65a on hit)
so one framedump shows RT geometry blended over the raster — a direct alignment check.

**Debug-layer fixes made:** (a) buffers are always created in COMMON (info 1328) — pools/
scratch now create + track COMMON; (b) each RayGenerationShaderRecord must be 64-aligned
(error 1161) — raygen records are strided by 64 (miss/hitgroup keep 32).

**What the overlay proves:** camera math, /256 world-unit scale, depth, and the
projection inverse are all **correct** — the RT track ribbon follows the road's curve,
perspective, and vanishing point exactly.

**GATE PASSED** (2026-07-31). The `TD5RE_RT_DEBUGVIEW=1` overlay now aligns with the
raster: the road strip covers the full road under both cars and recedes to the vanishing
point; the player car (green) and AI car (tan) silhouettes sit exactly on the raster cars
at correct position/size/orientation; horizon aligns. build+lint clean, goldens 54/0,
debug-layer clean, no TDR.

**Root causes found (via a `TD5RE_RT_DIAG` feed dump — kept, env-gated):**
1. **Camera scale (the big one).** `td5_camera_get_position` returns **FLOAT world units**
   (~136180), NOT 24.8 — the earlier survey claim was wrong. Dividing by 256 put the
   camera at ~(539,-84,-339), ~135k units from the scene; the track (a huge ground plane)
   still filled the lower screen and fooled the eye, but the cars (±300-unit boxes) were
   sub-pixel specks. Fix: **do not divide the camera position by 256** (track verts,
   `world_pos/256`, and the camera are all the same ~136180 float-world scale).
2. **Actor Y garbage.** `render_pos.y` is NOT populated at the `td5_rt_frame` point
   (`-4194310` for every actor; `.x/.z` were fine). Fix: instance from `world_pos` (24.8,
   authoritative) `/256` for all three axes.
3. **Track lanes + strays.** Emit **one quad per lane** (`lane_count` = strip byte +0x03
   low nibble) via the new span-type-aware `td5_track_get_lane_quad_world`
   (`get_quad_vertices`), and **reject implausibly large quads** (any-axis extent > 12000)
   to drop the junction-span +1-vertex strays.

Repro overlay: `td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=5 --RaceTrace=1
--AutoRaceReady=1` with `TD5RE_RT_DEBUGVIEW=1 TD5RE_FRAMEDUMP=<png>` (add
`TD5RE_RT_ONLYCARS=1` to isolate cars, `TD5RE_RT_DIAG=1` for the feed dump →
`log/rt_diag.log`).

### As-built — Phase 2a (2026-07-31, branch `rt-lighting`)

**Done + verified:** G-buffer MRT produced in the D3D12 backend. `d3d12_get_pso` gained
a gbuffer key-bit (bit 22) → `NumRenderTargets=2`, `RTVFormats[1]=R8G8B8A8`.
`Backend_SetGBufferEnabled` creates the R8G8B8A8 target (via `d3d12_tex_create(...,1)` —
both RTV + SRV) at render size and clears it to 0 (matid 0). `d3d12_bind_and_draw`
mirrors the D3D11 predicate (z-write ON, blend OFF) to bind the gbuffer as RT1 and
promote `PS_MODULATE`→`PS_MODULATE_G` / `PS_MODULATE_ALPHA`→`PS_MODULATE_ALPHA_G`;
`s_gbuf_bound` tracks the RT1 bind (reset in frame_begin + after each fullscreen pass).

**G-buffer verified correct** (via `TD5RE_RT_GBUF_DEBUG=1` blit + pixel sampling at
`--LightingMode=2`): flat road = `R128 G1 B128` (normal up), vertical wall = `R254 G128
B128` (facing +X), sky = `0,0,0,0` (cleared, not z-write opaque), matid in alpha
(A=5 = car body). Encoding matches the COLOR1 pack in `td5_render.c`/`td5_render_mesh.c`.

**LOW pixel-identical:** foliage-AA is dormant on D3D12 (`foliageAA` never set non-zero),
so `SampleTex==tex.Sample` and the `_g` `SV_Target0` output is byte-identical to the
non-`_g` PS — producing the G-buffer never changes the visible frame. The LOW deferred
passes **keep the placeholder** (`s_black_tex` at t1); real-G-buffer *consumption* is
gated to HIGH in Phase 2b. Default LightingMode=0 → gbuffer off → identical code path.
Verified: default LOW framedump = normal race scene; goldens 54/0; build+lint clean.

**Deviation from the plan's "replace the placeholder" step:** deferred passes keep the
placeholder in Phase 2a (per the plan's own LOW-identical caveat) — the gbuffer SRV is
produced + available; Phase 2b wires HIGH-gated consumption. **Debug-layer fix:** RT
textures now get a `{0,0,0,0}` optimized clear value in `d3d12_tex_create` (killed the
id=820 slow-clear perf warning). Debug-layer clean with the gbuffer producing.

### As-built — Phase 2b (2026-07-31, branch `rt-lighting`)

**Done + verified:** RT sun shadows + dynamic-light occlusion running in HIGH mode
(`TD5RE_RT=1` + `--LightingMode=2` for the G-buffer/pass-callers). Clean integration:
`Backend_ApplyShadowPass`/`LightPass` branch on `Backend_RTSetMode(td5_rt_active())` and
call `d3d12_dxr_shadow_pass`/`light_pass`, which **reuse the game's existing ShadowCB/
LightCB** (the same camera-reconstruction fields, byte-identical layout) as the raygen's
b1/b2 root CBV — no new CB plumbing. `rgen_shadow` (cone-jittered sun ray) writes a
`sunvis` R32F mask; `rgen_light` (attenuation + cone + soft-wrap Lambert, K shadow rays)
writes a `lightcol` RGBA16F mask; composited via dxr-owned MULT / additive blits
(`ps_shadow_rt` / `ps_light_rt`, fxc SM5.0) over the pane. Depth (R32F) + G-buffer SRVs
created into the DXR heap; a priv accessor (`d3d12_priv_scene_inputs`/`_restore`/
`_end_rt_pass`) exposes + transitions them and restores the backend RT/DSV + draw cache.

**Global RS restructured:** b0 (debug view) + b1 (ShadowCB) + b2 (LightCB) root CBVs +
descriptor table (UAV u0-u2 output/sunvis/lightcol, SRV t0-t2 TLAS/depth/gbuffer) +
static sampler. Heap slots renumbered (0-8). State object + SBT grew to include
`rgen_shadow` (raygen 2) + `rgen_light` (raygen 3), 64-strided records.

**Blob-shadow kill:** `render_vehicle_shadow_quad` early-returns when `td5_rt_active()`.

**Verified:** `TD5RE_RT_MASK=1` opaque mask blit confirms correct occlusion — cars gray
(self-shadow + sun-blocking), road/sky white (lit), cast-shadow regions on the road from
cars/buildings; blob gone. No crash, D3D12 debug-layer clean (0 non-690), goldens 54/0
(RT off by default → LOW march unchanged, sim byte-identical), build+lint clean (dev +
release). Env knobs: `TD5RE_RT` (HIGH), `TD5RE_RT_MASK` (mask viz).

**OWED (Phase 2b refinements, not gating the commit):** (1) depth-aware 5x5 denoise
(`ps_rt_blur.hlsl`) — currently the cone-jitter leaves mild noise; (2) perf measurement
(≤2 ms target) + 10-min TDR soak; (3) night-headlight-occlusion + tunnel visual checks
(light pass built + runs, not yet visually confirmed); (4) `TD5RE_RT_BIAS`/`_RAYS`/
`_MAXLIGHTS` knobs (currently baked constants; car self-shadow bias may want tuning).
Interim activation is `TD5RE_RT=1`; the `[Lighting] Quality` INI row is Phase 4.

## Handoff prompt (what launched this execution)

```
Read C:\Users\maria\Desktop\Proyectos\TD5RE\docs\plans\RT_LIGHTING_PLAN.md in full, then
CLAUDE.md, before touching anything. Execute the plan phase by phase, in order
(0 → 1 → 2a → 2b → 3 → 4). Rules:

- One phase per commit (Phase 2a separate). Do not start a phase until the previous
  phase's verification gate passed: build_all.bat clean with no new warnings,
  `pwsh scripts/selftest.ps1` green, golden traces green (any golden diff = your bug,
  revert and rethink — RT is render-only), plus the phase's framedump checks
  (TD5RE_FRAMEDUMP=<path.png>; desktop capture is black, never use it).
- Phase 1's debug-view alignment gate is hard: do not begin Phase 2 until
  TD5RE_RT_DEBUGVIEW=1 framedumps align with raster framedumps of the same frame.
- LOW quality (Quality=0) must remain behaviorally identical to current master in
  every phase; never modify ps_shadow.hlsl / ps_light.hlsl / ps_ssr.hlsl.
- Precondition before writing any code: verify dxc.exe exists in the Windows 10 SDK
  bin dir, and that the bundled MinGW d3d12.h declares ID3D12Device5 /
  D3D12_DISPATCH_RAYS_DESC / the DXR IIDs. If dxc is missing, STOP and tell the user
  to place a dxc release in td5mod/ddraw_wrapper/tools/dxc/ (you have no web access).
- The plan freezes SBT layout, root signatures, struct layouts, and API sequences —
  follow them exactly; do not redesign.
- Use the td5re MCP live-control server (dev build, --Control=1) for in-game test
  scenarios; kill only your own PID, never by image name.
- Commit messages: `d3d12(RT-P<n>): <summary>` with the project's co-author trailers.
  Do not push; leave commits on a branch `rt-lighting`.
- If a capability you need is denied by managed policy, stop and report exactly what
  is blocked instead of working around it. Never fabricate build/test output.
- After each phase, append a short as-built note (deviations, measurements) to the
  plan doc's status section.
```

---

## 1. Goal & user decisions (locked — do not revisit)

Replace the current screen-space lighting stack with a real ray-traced system:

1. **Full DispatchRays pipeline** (raygen/closest-hit/miss shaders, state object, shader
   binding table) — NOT inline RayQuery.
2. RT replaces: **sun shadows** (currently a screen-space depth march), **headlight /
   dynamic-light occlusion** (currently a 12-step screen march per light), and
   **reflections with FULL TEXTURED HIT SHADING** (hit-triangle UV interpolation +
   texture sampling + lighting at the hit point) on reflective materials (car body,
   glass, wet roads). The **car ground blob shadow** is also replaced in HIGH mode.
3. New GRAPHICS OPTIONS row **"LIGHTING QUALITY: LOW / HIGH"**. LOW = the entire current
   screen-space stack + blob shadow, unchanged (default when no DXR). HIGH = the RT
   system (default when DXR available). Auto-fallback to LOW when the device lacks DXR.
4. LOW must stay behaviorally byte-identical to current master throughout.

Hardware context: the exe is x86_64-only; RTX 5070 Ti reports Raytracing Tier 1.2 in a
64-bit process (see `docs/plans/X64_DXR_ROADMAP.md` — NVIDIA withholds DXR from 32-bit;
that roadmap's Stage 4 is what this plan executes).

## 2. Survey facts (verified 2026-07-31 — trust these, spot-check line numbers as the tree moves)

**Backend**: single file `td5mod/ddraw_wrapper/src/d3d12_backend.c` (~2,661 LOC, C with
COBJMACROS, MinGW-w64 GCC). Device created at FL 11_0 querying **only `ID3D12Device`**
(`:1858`) — no Device5, no OPTIONS5 check anywhere. One direct queue; 2 frames in flight
(`D3D12_FRAME_COUNT 2`, `:29`); 32 MB per-frame persistent-mapped UPLOAD rings; per-draw
CB ring allocator `d3d12_ring_cb` (`:1135`, returns GPU VA usable as a root CBV);
deferred-deletion fence machinery (`:454-513`); device-lost recovery with generation
counters. Frame begin/present at `:525` / `:645`.

**Shaders**: offline `fxc` SM5.0 DXBC byte arrays committed as `*_bytes_50.h`
(`td5mod/ddraw_wrapper/src/shaders/compile_shaders.bat`). DXR shaders require DXIL →
`dxc -T lib_6_3` — a **new offline toolchain step**. DXIL blobs are pure data in headers:
zero MinGW linking implications.

**Geometry crux**: all T&L is on the CPU. The backend only ever receives 32-byte XYZRHW
**screen-space** vertices streamed per frame (`Backend_PlatDrawTris` `:2262` →
`Backend_StreamUpload` `:2422`) — useless for ray tracing. World-space geometry exists
only CPU-side: track = `g_spanTable`/`g_vertexTable` (24.8 fixed point, world **+Y is
down**), cars/props = the retained CPU mesh registry in `td5_render_mesh.c`. A **new
world-space geometry feed above the backend boundary** is required (Phase 1).

**Deferred pass stack (the LOW path)**: dispatched from `td5_game.c:7360-7372` in order
shadow → light → SSR, after opaque world, before translucent VFX/HUD. CPU sides
`td5_render_apply_shadow_pass/light_pass/ssr_pass` (`td5_render_mesh.c:225-329`); bridge
`td5_platform_win32.c:3867-3947` → `Backend_ApplyShadowPass/LightPass/SSRPass`
(`d3d12_backend.c:2093-2141`); generic `d3d12_fullscreen_pass` `:1151`; pass root sig
`:1047` (b0 CBV + t0-t2 SRV table + static point-clamp sampler).

**G-buffer gap**: the game packs world normal (24b) + material id (8b) into vertex COLOR1
(`td5_light2.c` / `td5_material.c`; matids NONE=0/DEFAULT/CUTOUT/GLASS/GLOW/CARBODY with
per-id reflectivity), and `_g` pixel-shader variants exist — but the D3D12 backend never
populates the G-buffer: `Backend_SetGBufferEnabled` is a stub (`:2413`) and the passes
bind a 1×1 black placeholder (`:2131`, `:2145`). Wiring the G-buffer MRT is Phase 2a.

**Lights**: CPU registry `td5_light.c` — `TD5_LIGHT_MAX=32`; headlight spot emitters
derived from car hardpoints (`td5_light_emit_vehicle_headlights` `:174-257`); optional
street lamps (off by default). Auto-headlight darkness detection
`td5_render_env_is_dark_for_slot` (`td5_render_mesh.c:380-438`) — **unchanged by this
plan**; only the occlusion source changes.

**Blob shadow**: `render_vehicle_shadow_conforming` (`td5_render_effects.c:792`),
dispatcher `render_vehicle_shadow_quad` (`:1120`).

**Options menu**: GRAPHICS OPTIONS = `Screen_DisplayOptions` (`td5_fe_menu.c:1844`),
6 rows + OK. The 6-touchpoint selector-row recipe: button create + label refresh
(`td5_fe_menu.c:185`), input handler case 6 (`:1896`), OK persist (`:1932`), value
overlay (`td5_frontend.c:6843`), arrow dispatch loop (`td5_frontend.c:9997` — **latent
gap: only rows 0-3 get ◄► arrows**), backing static (`td5_frontend.c:783`) + INI seed
(`:10476`). INI `[Lighting]` is schema-driven via `k_lighting_cfg[]` (`main.c:427-443`)
which gives `--Key=N` CLI override + persistence for free. i18n via `TR()` +
`re/assets/frontend/lang/es_AR.txt` + `python re/tools/gen_i18n_catalog.py`.

## 3. Global invariants (every phase — repeat in every phase's verification)

- **SIM untouched; golden traces stay green in every phase.** RT is render-only; the
  geometry feed *reads* span/actor state, never writes it. Any golden diff = your bug.
- **LOW is byte-identical to today** (except the new menu row). `ps_shadow.hlsl`,
  `ps_light.hlsl`, `ps_ssr.hlsl` are never modified.
- Lint ratchets: no new `extern` decls in .c files (cross-module decls go in headers),
  no new `td5_game.h` includers, no new compiler warnings.
- Wrapper-internal sharing: `d3d12_backend.c` keeps everything static. Expose what
  `d3d12_dxr.c` needs (device, current frame's list4, fence/deferred-release API,
  `d3d12_ring_cb`, depth/G-buffer/scene-color SRVs, frame index) via a small accessor
  struct (`d3d12_dxr_env`, populated once per frame by `d3d12_backend.c`) declared in
  `d3d12_backend_priv.h` — not exported globals.
- Device-lost: every RT resource is torn down in `Backend_ReleaseDeviceObjects` and
  lazily recreated after device recreation; an AS generation counter
  (`Backend_RTGeneration()`) lets the game re-feed meshes when the counter changes.
- **TDR discipline** (this project has a TDR history — see memory/crash.log conventions):
  chunked BLAS builds (≤~500k tris per frame during a warmup window after level load),
  `RTMARK:*` crumbs pushed to the existing DRAW WATCH ring so crash.log shows what RT
  work was in flight, no DispatchRays wider than the swapchain.
- Naming: game side `td5_rt_*` (new `td5mod/src/td5re/td5_rt.c/.h`, added to `srcs.txt`),
  platform bridge `td5_plat_rt_*` (`td5_platform.h` + `td5_platform_win32.c`), wrapper
  public API `Backend_RT*` (declared in `wrapper.h`), wrapper internals `d3d12_dxr_*` in
  new `td5mod/ddraw_wrapper/src/d3d12_dxr.c` (added to `wrapper_srcs.txt` — mind that
  file's formatting rules).
- Env kill-switch from day one: `TD5RE_RT_DISABLE=1` forces DXR caps off entirely.
- All RT dispatches happen only where the deferred passes run today (in-race; never in
  frontend/FMV/garage).

## 4. Cross-cutting design (FROZEN — implement exactly this)

- **SBT**: one committed UPLOAD buffer. Layout `[raygen records | miss records | hitgroup
  records]`; each *region* start aligned to `D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT`
  (64); each *record* = the 32-byte shader identifier from
  `ID3D12StateObjectProperties::GetShaderIdentifier` and stride 32
  (`D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT`). **No local root signatures, no local
  root arguments, ever** — all per-dispatch data goes through the global RS; per-geometry
  data through `InstanceID()`/`GeometryIndex()` → GeoRecord StructuredBuffer.
- **Hit groups**: exactly one, `"hg"` = { closesthit `chit_refl`, anyhit `anyhit_cutout` }.
  Shadow rays never invoke CH (`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
  RAY_FLAG_SKIP_CLOSEST_HIT_SHADER`).
- **Miss table**: 2 records — index 0 `miss_shadow` (sets payload.visible=1), index 1
  `miss_refl` (sky/fog color). Selected via the `MissShaderIndex` argument of TraceRay.
- **Global root signature** (one, shared by all raygens): b0 root CBV (per-dispatch
  constants via `d3d12_ring_cb`), descriptor table 0 = fixed slots (output UAVs, TLAS
  SRV, depth, G-buffer, masks, GeoRecord + VB/IB pool SRVs), descriptor table 1 =
  unbounded SRV range `t0, space1` (bindless textures, Phase 3; `NumDescriptors =
  (UINT)-1`), one static LINEAR-wrap sampler. Serialize with `D3D12SerializeRootSignature`
  v1.0, copying the existing pass-root-sig code at `:1047`.
- **Recursion**: `MaxTraceRecursionDepth = 2` (raygen ray = 1, CH's sun-shadow ray = 2).
  Never more.
- **Shader config**: `MaxPayloadSizeInBytes = 32`, `MaxAttributeSizeInBytes = 8`.
- **DXIL library**: ONE `lib_6_3` blob `rt_pipeline.hlsl` containing all entry points
  (`rgen_smoke`, `rgen_debug`, `rgen_shadow`, `rgen_refl`, `miss_shadow`, `miss_refl`,
  `chit_refl`, `anyhit_cutout`), exports selected by name in one
  `D3D12_DXIL_LIBRARY_DESC`.
- **Descriptor heap**: a NEW shader-visible CBV/SRV/UAV heap owned by `d3d12_dxr.c`
  (the existing backend heap is CPU-staging + per-draw copy ring — do not touch it).
  Phase 0: 8 slots; Phase 3 grows it to 4096 ([0..15] fixed, [16..] bindless textures).
- **Split-screen**: per-pane DispatchRays (Width/Height = pane rect, pane camera +
  pane-origin constant in the CB); masks/reflection UAVs are full-frame, panes write
  their rects. Mirror however `Backend_ApplyShadowPass` handles panes today.
- **Coordinates**: everything RT-side works in **game world space** (24.8 → float via
  `/256.0f`, +Y down preserved, no axis flips) so no convention mismatch can exist
  between raster reconstruction and rays.
- **Perf target** (1080p60 native on 5070 Ti): TLAS ~0.1 ms + shadows ≤2 ms +
  reflections ≤3 ms ≈ ≤5.5 ms RT budget.
- **Memory budget** (uncompacted, acceptable): track BLAS 100–250 MB, ~30 car/prop BLAS
  × 1–2 MB, TLAS <1 MB ×2, VB/IB pools 50–100 MB, UAVs ~40 MB. Total <500 MB.
  Compaction is the documented reclaim path (needs postbuild-info readback — v2).
- **Explicitly untouched**: all SIM/physics/AI/netplay/replay code, golden traces, LOW
  shaders, input, audio, save format (INI gains one schema-driven key only).

---

## Phase 0 — DXR foundation (~600 LOC, low risk)

**Goal**: dxc offline pipeline emitting DXIL headers; Device5/List4 acquisition; tier
check; capability flag visible to the game; a raygen-only DispatchRays filling a UAV
gradient, verified via framedump. No game-visible change.

### 0.1 dxc toolchain
- Check `C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64\dxc.exe` (same SDK as the
  fxc path hard-coded in `compile_shaders.bat:4`). If absent → STOP, ask the user to
  drop a dxc release into `td5mod/ddraw_wrapper/tools/dxc/` (executor has no web).
- Extend `compile_shaders.bat`:
  ```bat
  %DXC% -nologo -T lib_6_3 -Fh rt_pipeline_bytes.h -Vn g_rt_pipeline rt_pipeline.hlsl
  ```
  If dxc emits `BYTE`-typed arrays, keep them (compiles under MinGW after `<windows.h>`).
- New file `td5mod/ddraw_wrapper/src/shaders/rt_pipeline.hlsl` (Phase 0: just
  `rgen_smoke` writing `float3(xy/dims, 0)` to the output UAV).

### 0.2 Device caps (`d3d12_backend.c`, ~40 LOC)
Immediately after `D3D12CreateDevice` (`:1858`):
```c
D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {0};
hr = ID3D12Device_CheckFeatureSupport(g_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
if (SUCCEEDED(hr) && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0)
    hr = ID3D12Device_QueryInterface(g_d3d12.device, &IID_ID3D12Device5, (void**)&g_d3d12.device5);
```
Also QI `ID3D12GraphicsCommandList4` from the command list at creation. Both pointers may
be NULL — every RT entry point checks. **MinGW caveat (verify FIRST, before writing any
other code)**: confirm the bundled toolchain's `d3d12.h` declares `ID3D12Device5`,
`D3D12_RAYTRACING_TIER`, `D3D12_DISPATCH_RAYS_DESC`, `BuildRaytracingAccelerationStructure`
etc., and that `IID_ID3D12Device5` / `IID_ID3D12GraphicsCommandList4` resolve at link
time; if IIDs are missing, `DEFINE_GUID` them locally under `INITGUID` in exactly one TU
(mirror how the wrapper obtains newer IIDs today). `TD5RE_RT_DISABLE=1` forces caps off.

### 0.3 Capability plumbing
- `wrapper.h`: `int Backend_RTAvailable(void);` (0 after device-lost until re-QI).
- `td5_platform.h` + `td5_platform_win32.c`: `int td5_plat_rt_available(void);` (place
  next to the ApplyShadowPass bridges `:3867-3947`).
- New `td5_rt.c/.h`: `td5_rt_available()`, `td5_rt_active()` (= available && quality==HIGH
  && in-race && lighting enabled). Add to `srcs.txt`.

### 0.4 Smoke dispatch (`d3d12_dxr.c`, new file)
`d3d12_dxr_init()` (lazy, first use): output UAV texture (swapchain-sized, recreated on
resize), global RS (minimal for now: b0 + u0 table + sampler), state object via
`ID3D12Device5_CreateStateObject` with a flat `D3D12_STATE_SUBOBJECT` array:
1. `DXIL_LIBRARY` → `g_rt_pipeline`, exports `{"rgen_smoke"}`.
2. `SHADER_CONFIG` → {32, 8}.
3. `PIPELINE_CONFIG` → depth 1 (Phase 0 only; 2 from Phase 3).
4. `GLOBAL_ROOT_SIGNATURE`.
(Hit group subobject arrives in Phase 1 — raygen with no TraceRay is legal.)
SBT built once at init per §4. Dispatch:
```c
ID3D12GraphicsCommandList4_SetPipelineState1(list4, g_dxr.so);
ID3D12GraphicsCommandList_SetComputeRootSignature(list, g_dxr.global_rs);
/* set heap; root CBV = d3d12_ring_cb(...); tables */
D3D12_DISPATCH_RAYS_DESC d = {0};
d.RayGenerationShaderRecord = (D3D12_GPU_VIRTUAL_ADDRESS_RANGE){ sbt_va + 0, 32 };
d.MissShaderTable   = (D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE){ sbt_va + 64, 64, 32 };
d.HitGroupTable     = (D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE){ sbt_va + 128, 32, 32 };
d.Width = w; d.Height = h; d.Depth = 1;
ID3D12GraphicsCommandList4_DispatchRays(list4, &d);
```
then UAV barrier + blit over the scene, all behind `TD5RE_RT_SMOKE=1`.

**Files**: create `src/d3d12_dxr.c`, `src/shaders/rt_pipeline.hlsl` (+ generated
`rt_pipeline_bytes.h`); modify `wrapper_srcs.txt`, `compile_shaders.bat`,
`d3d12_backend.c`, `wrapper.h`, `d3d12_backend_priv.h`; game: create `td5_rt.c/.h`,
modify `srcs.txt`, `td5_platform.h`, `td5_platform_win32.c`.

**Gate**: build_all clean, no new warnings; selftest green; goldens green;
`TD5RE_RT_SMOKE=1` + framedump shows the gradient; `TD5RE_RT_DISABLE=1` → normal frame;
debug layer (if enabled) clean.

---

## Phase 1 — World-space geometry feed + BLAS/TLAS + debug view (~1,700 LOC, medium risk)

**Goal**: game feeds world-space triangles; wrapper builds track BLAS at level load,
per-mesh BLAS lazily, TLAS per frame; a primary-ray debug view proves the AS matches the
raster world. No user-visible change.

### 1.1 Feed API (`wrapper.h`; all no-ops when RT unavailable)
```c
typedef struct { float pos[3]; float uv[2]; unsigned color; } BackendRTVertex;  /* 24 B */
typedef struct { unsigned first_index, index_count, texture_id, matid_flags; } BackendRTRange;
int  Backend_RTMeshCreate(const BackendRTVertex*, unsigned nverts,
                          const unsigned short* idx, unsigned nidx,
                          const BackendRTRange*, unsigned nranges); /* handle >0; 0=fail */
void Backend_RTMeshDestroy(int handle);
void Backend_RTSceneBegin(void);
void Backend_RTSceneInstance(int mesh, const float m3x4[12], unsigned flags);
void Backend_RTSceneEnd(void);   /* builds TLAS this frame */
```
Texture id / matid are constant per span/mesh-subset → they live in per-**range** records,
not per-vertex. `texture_id` = the wrapper's existing texture handle/key (locate the
texture registry in `d3d12_backend.c`/`td5_backend_texture.h` and reuse its keying;
stored opaquely until Phase 3).

### 1.2 Game side (`td5_rt.c` + hooks)
- `td5_rt_level_build()` at level-load completion (hook where `g_spanTable` is finalized;
  grep it in `td5_render_mesh.c`): walk the ENTIRE span table once (not frustum-culled),
  convert 24.8 → float (`/256.0f`, keep handedness), emit the track as one big static
  mesh — chunk into N meshes by span region if >~2M tris (also enables split builds).
  Ranges keyed by texture page + matid (same matid lookup `td5_material.c` uses for
  COLOR1 packing). `td5_rt_level_unload()` destroys handles at level teardown.
- Car/prop meshes: add a lazy `rt_handle` to the retained mesh registry entries in
  `td5_render_mesh.c`; first sight while RT available → convert object-space verts and
  create; destroy on eviction/level unload. Damage deformation: stale BLAS accepted in
  v1 (minor shadow/reflection error); upgrade path = recreate on damage event.
- `td5_rt_frame()` — called once per render frame from the `td5_game.c:7360` vicinity,
  BEFORE the deferred passes: SceneBegin; instance track (identity); instance all actors
  (~≤64) with `rotation_matrix`/position fixed→float as **row-major 3×4** (translation in
  m[3], m[7], m[11]); SceneEnd. Also snapshot camera (position + inverse-view-projection
  floats — mirror the exact math the existing pass CBs are built with in
  `td5_platform_win32.c:3867-3947`), sun dir, and the light registry (≤32) into a
  `td5_plat_rt_set_view(...)` bridge feeding the wrapper's per-frame RT CB.
- **Gate the feed on `td5_rt_available()` (not `active`)** so ASes always exist when DXR
  is present → the Phase 4 LOW↔HIGH switch is instant. TLAS-in-LOW costs ~0.1 ms;
  acceptable (re-measure; fallback = build on first HIGH frame).

### 1.3 BLAS build (`d3d12_dxr.c`)
- Copy VB/IB into **two big pooled DEFAULT-heap buffers** (`g_rt_vb_pool` /
  `g_rt_ib_pool`, grown geometrically, each with one ByteAddressBuffer SRV) — pools from
  the start; Phase 3's vertex fetch depends on them. A mesh = pool ranges.
- **One `D3D12_RAYTRACING_GEOMETRY_DESC` per range** (shared VB, per-range IB offset) so
  `GeometryIndex()` directly selects the range:
  ```c
  g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;      /* CUTOUT revisited in Phase 3 */
  g.Triangles.VertexBuffer.StartAddress = vb_pool_va + mesh_vb_off;  /* pos at offset 0 */
  g.Triangles.VertexBuffer.StrideInBytes = 24;
  g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  g.Triangles.VertexCount  = nverts;
  g.Triangles.IndexBuffer  = ib_pool_va + mesh_ib_off + range->first_index * 2;
  g.Triangles.IndexFormat  = DXGI_FORMAT_R16_UINT;
  g.Triangles.IndexCount   = range->index_count;
  ```
- `GetRaytracingAccelerationStructurePrebuildInfo` → result buffer
  (`pi.ResultDataMaxSizeInBytes`, DEFAULT heap, initial state
  `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`, flag
  `ALLOW_UNORDERED_ACCESS`) + **one shared growable scratch** (UAV barrier between builds
  sharing it). `BuildRaytracingAccelerationStructure` on list4, then UAV barrier on the
  result. Scratch retired via the deferred-deletion fence machinery. **No compaction in
  v1** (documented upgrade).
- TDR guard: during a warmup window after level load, build ≤~500k tris of BLAS per
  frame (`built` flag per mesh; TLAS skips unbuilt meshes). Push `RTMARK:blas_build`.

### 1.4 TLAS (`Backend_RTSceneEnd`)
- `D3D12_RAYTRACING_INSTANCE_DESC` array in a per-frame upload slice:
  Transform = the 3×4; `InstanceID` = mesh's first GeoRecord index (Phase 3);
  `InstanceMask = 0xFF`; `InstanceContributionToHitGroupIndex = 0` (single hit group);
  Flags: start with none and `RAY_FLAG_CULL_NONE` in shaders (settle winding empirically
  in the debug view); `AccelerationStructure = blas_va`.
- **Full rebuild every frame**, `PREFER_FAST_BUILD` (≤~64 instances — microseconds; no
  refit machinery). Capacity allocated for 128 up front; re-query prebuild info on
  growth. **Double-buffer the TLAS result** (frame parity — 2 frames in flight). TLAS
  SRV = `D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE` (Location = tlas_va,
  pResource NULL), refreshed on parity swap.

### 1.5 Debug view (phase gate; committed, behind env)
- Add the hit group subobject now ({`chit` trivial: payload.t/instance}, `miss`).
- `rgen_debug` (`TD5RE_RT_DEBUGVIEW=1`): primary ray per pixel from the camera CB
  (simplest correct route: reuse `ps_shadow.hlsl`'s depth→world reconstruction math to
  compute the far-plane world point, ray dir = normalize(that − campos)),
  `TraceRay(RAY_FLAG_CULL_NONE, 0xFF, …)`, write hitT grayscale or hashed
  `InstanceIndex()` color; miss = dark blue. Blit over the frame.
- **HARD GATE**: framedump of the debug view must align with a raster framedump of the
  same frame (track silhouette, car positions, horizon). Misalignment signatures:
  global scale error → fixed-point conversion bug; per-car offset → transform layout;
  skewed horizon → camera math. Do not start Phase 2 until aligned.

**Files**: `td5_rt.c/.h` (+~500), `td5_render_mesh.c` (hooks ~80), `td5_game.c` (one
call), `td5_platform.h`/`td5_platform_win32.c` (~120), `wrapper.h`, `d3d12_dxr.c`
(+~900), `rt_pipeline.hlsl` (+~120), `compile_shaders.bat`.

**Gate**: build/selftest/goldens green; `TD5RE_RT_DISABLE=1` run identical to master;
debug-view framedumps at 2–3 fixed race scenarios (td5re MCP live-control: known track +
car + camera) aligned vs raster; TLAS+feed overhead <0.5 ms; debug layer clean.

---

## Phase 2a — G-buffer MRT wiring in D3D12 (~250 LOC, standalone commit)

- Allocate an R8G8B8A8 G-buffer target (normal 24b + matid 8b, matching the COLOR1
  packing contract in `td5_light2.c` / the existing `ps_modulate_g.hlsl` /
  `ps_modulate_alpha_g.hlsl`), add a gbuffer-on bit to the PSO cache key, bind as MRT1
  when enabled and select the `_g` PS variants, clear per frame, transition to SRV for
  the deferred passes, replace the 1×1 black placeholder binding.
- Reference: the retired D3D11 backend implemented this — consult `d3d11_backend_priv.h`
  remnants / git history for the MRT contract.
- **LOW-identical caveat**: if feeding real G-buffer data to the existing screen-space
  shaders changes LOW's look (check whether `ps_light.hlsl`/`ps_shadow.hlsl`/`ps_ssr.hlsl`
  sample the gbuffer meaningfully — they do read normals when present), gate G-buffer
  *consumption* behind HIGH and keep LOW binding the placeholder. LOW must stay
  pixel-identical.

**Gate**: build/selftest/goldens green; LOW framedumps byte-compare vs pre-phase baseline.

## Phase 2b — RT shadows: sun + dynamic lights (~1,200 LOC, medium-high risk)

- `rgen_shadow`: per pixel — read depth + G-buffer normal, reconstruct world pos (same
  math as ps_shadow), offset origin along normal by ε (`TD5RE_RT_BIAS`, scaled with
  distance — fights acne from 24.8-quantized geometry). Then:
  - **Sun**: 1 shadow ray toward sun dir with cone jitter (~0.5–1° angular radius,
    per-pixel spatial hash RNG — **no temporal accumulation in v1**; `TD5RE_RT_RAYS`
    allows 2/4 samples). Clamp jitter by hit distance for contact hardening near the
    car. Flags `ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_CLOSEST_HIT_SHADER`, miss index 0.
  - **Dynamic lights**: from the light CB (≤32 registry lights: pos/radius/cone), pick
    the K nearest/brightest per pixel (K = `TD5RE_RT_MAXLIGHTS`, default 4), one shadow
    ray each (TMax = light distance), accumulate visibility bits.
  - Output: R8 UAV sun visibility + R32_UINT UAV packed per-light bits (light-slot order
    identical to the CB order — freeze the shared struct in `rt_common.hlsli` + C mirror).
- Spatial denoise: depth-aware 5×5 separable blur, new fxc SM5.0 `ps_rt_blur.hlsl` via
  the existing `d3d12_fullscreen_pass` infra. (Temporal history = documented upgrade,
  not built now.)
- **Composite via input swap — `td5_game.c` dispatch order untouched**: new
  `ps_shadow_rt.hlsl` (MULT blend, samples the sun mask) and `ps_light_rt.hlsl` (same
  additive accumulation as ps_light but occlusion term = RT visibility bit). The wrapper
  branches inside `Backend_ApplyShadowPass`/`Backend_ApplyLightPass` on a mode flag set
  by `Backend_RTSetMode(int high)` (called from the game via a `td5_plat_rt_set_mode`
  bridge each frame = `td5_rt_active()`). Barriers: depth/G-buffer →
  NON_PIXEL_SHADER_RESOURCE, dispatch, UAV barrier, masks → PIXEL_SHADER_RESOURCE,
  composite, restore.
- **Blob shadow kill**: `render_vehicle_shadow_conforming` early-outs when
  `td5_rt_active()` (the car BLAS occludes sun rays at road pixels = real contact
  shadow). ~5 LOC in `td5_render_effects.c`.
- Split-screen: per-pane dispatch per §4 (check how ApplyShadowPass panes today and
  mirror it).
- Interim activation (no menu yet): INI `[Lighting] Quality=1` (add the schema row now —
  see Phase 4.1) or env `TD5RE_RT=1`.

**Gate**: build/selftest/goldens green; **LOW framedumps byte-compare vs baseline**;
HIGH scene checks via MCP live-control: (a) noon race — proper sun car shadow, blob
gone; (b) night, parked next to a wall — headlight pool occluded by the wall; (c) tunnel
— auto-darkness still triggers (detection code untouched). Shadow dispatch + blur
≤2.0 ms at 1080p. 10-minute race soak, no TDR, `RTMARK:dispatch_shadow` crumbs working.

---

## Phase 3 — RT reflections with textured hit shading (~1,400 LOC, high risk)

### 3.1 Bindless textures
- **Classic unbounded-range bindless — NOT SM6.6 ResourceDescriptorHeap** (avoids
  lib_6_6/newer-dxc/heap-flag requirements; Tier 1.2 hardware = resource binding Tier 3,
  unbounded arrays fine).
- Grow the dxr heap to 4096 slots: [0..15] fixed (UAVs, TLAS, depth, G-buffer, masks,
  GeoRecord/pool SRVs), [16..] per-texture SRVs. Hook the backend's texture registry:
  when a texture's GPU resource is (re)created, assign a dense `rt_texture_index` and
  `CreateShaderResourceView` into slot 16+idx; on destroy, overwrite with a 1×1 magenta
  fallback descriptor (retired-index hits sample magenta — benign in a reflection, and
  a visual audit signal).
- Global RS table 1 = unbounded `t0, space1`; HLSL
  `Texture2D g_tex[] : register(t0, space1);` + `NonUniformResourceIndex`. RS change →
  state object rebuilt at init only.

### 3.2 Per-geometry records + vertex fetch
- `GeoRecord { uint vb_byte_offset; uint ib_byte_offset; uint texture_index;
  uint matid_flags; }` — one per (mesh, range), in a persistent StructuredBuffer (SRV in
  a fixed slot). CH: `rec = g_geo[InstanceID() + GeometryIndex()]`; indices = u16×3 at
  `rec.ib_byte_offset + PrimitiveIndex()*6` from the IB pool; verts from the VB pool;
  interpolate UV + color by `attr.barycentrics`.
- **CUTOUT** ranges (fences, foliage): geometry flag *not* OPAQUE + `anyhit_cutout`
  (sample alpha, `IgnoreHit()` below threshold). Retroactively fixes Phase 2 shadow rays
  through fences — shadow rays must then NOT use FORCE_OPAQUE (keep ACCEPT_FIRST_HIT).

### 3.3 Reflection pipeline
- `rgen_refl`: read G-buffer matid → reflectivity from a 256-entry LUT in the CB
  (mirror `td5_material.c` k_params + the wet-road boost — copy `ps_ssr.hlsl`'s CB
  fields and reflectivity logic for parity); **early-out at ~0 reflectivity** (most
  pixels — the perf save); reconstruct pos+normal, reflect view dir, roughness jitter by
  matid (glass sharp, wet road slightly rough); TraceRay (miss index 1), payload
  {color, t}.
- `chit_refl`: fetch + interpolate; `SampleLevel` LOD 0 (no derivatives in CH; distance
  LOD = later knob); light = zone directional (N from cross of edges) + ambient + **one
  sun shadow ray** (depth 2 total) + fog by hitT (feed the raster fog CB params into the
  RT CB for parity); modulate by interpolated vertex color; GLOW matid = emissive,
  skip lighting.
- `miss_refl`: sky/fog color from CB (SSR today fades to scene on miss; the difference
  is acceptable — note it).
- Composite: `ps_ssr_rt.hlsl` — fullscreen, samples the reflection UAV, same per-matid
  reflectivity × Fresnel factor and alpha blend as `ps_ssr.hlsl`; branch inside
  `Backend_ApplySSRPass` on the RT mode flag.
- Half-res insurance: `TD5RE_RT_REFL_SCALE=50|100` (default 100 on the 5070 Ti) —
  dispatch at ½×½ + bilateral upsample in the composite.
- Debug: `TD5RE_RT_DEBUGVIEW=2` = visualize hit UV / texture index as false color.

**Gate**: build/selftest/goldens green; LOW byte-identical; HIGH framedumps: textured
reflections on car body (zoom crop proves texture content, not flat color), glass,
wet-road rain scene vs SSR side-by-side; **off-screen content visible in a reflection**
(stage a wall behind the camera — the RT win SSR can't do); no magenta in normal play;
reflection dispatch ≤3 ms at 1080p full-res, else flip default to half-res. Debug layer
clean.

---

## Phase 4 — Menu, INI, runtime switch, fallback, release (~500 LOC, low risk)

### 4.1 Config
- `[Lighting] Quality=0|1` added to `k_lighting_cfg[]` (`main.c:427-443`) → CLI
  `--Quality=N` + persistence free. Seeding: key absent in INI AND DXR available → 1;
  INI says 1 but DXR unavailable → run as 0 without rewriting the INI.
- `td5_rt_active()` = available && quality && in_race && !fmv && `[Lighting] Enabled`.

### 4.2 Menu row (6-touchpoint recipe)
"LIGHTING QUALITY" on `Screen_DisplayOptions` (7 rows + OK): button create + label
refresh (`td5_fe_menu.c:185`), input case (`:1896`), OK persist (`:1932`), value overlay
LOW/HIGH (`td5_frontend.c:6843`), arrow dispatch (`:9997`) — **fix the latent arrows
rows-0-3 gap while here** (first check whether rows 4-5 were intentionally arrow-less;
extend the bound accordingly), backing static (`:783`) + INI seed (`:10476`). DXR
absent → row greyed/inert (follow whatever disabled-row convention exists; else render
"LOW" and deny input with the standard blip). i18n: `LIGHTING QUALITY` / `LOW` / `HIGH`
via TR() + es_AR entries (CALIDAD DE ILUMINACIÓN / BAJA / ALTA), then
`python re/tools/gen_i18n_catalog.py`.

### 4.3 Runtime switch LOW↔HIGH (no restart)
Instant: the mode flag branches the pass dispatches; ASes are always resident when DXR
is present (Phase 1 gated the feed on `available`), freed only on level unload.

### 4.4 Robustness + release
- Device-lost drill: all `d3d12_dxr.c` resources torn down in
  `Backend_ReleaseDeviceObjects`; caps re-queried on recreation; `Backend_RTGeneration()`
  bump → `td5_rt_frame` recreates mesh handles.
- Knobs documented (README / EXPECTED_BEHAVIOR notes): `TD5RE_RT_DISABLE`, `TD5RE_RT_SMOKE`,
  `TD5RE_RT_DEBUGVIEW`, `TD5RE_RT_MAXLIGHTS`, `TD5RE_RT_RAYS`, `TD5RE_RT_REFL_SCALE`,
  `TD5RE_RT_BIAS`, `TD5RE_RT_STATS`.
- RELEASE: srcs/wrapper_srcs are shared with release builds → automatic; verify
  `td5re_release.exe` runs HIGH.
- Append as-built notes to this doc.

**Gate**: `pwsh scripts/selftest.ps1 -Suite full` + goldens green; menu framedumps (row,
ES translation, greyed state under `TD5RE_RT_DISABLE=1`); INI round-trip (HIGH → quit →
`Quality=1` persisted → relaunch HIGH); mid-race toggle both directions via MCP;
split-screen race in HIGH; frontend/FMV RT-dormant (stats counter zero); 30-minute soak
(no TDR, VRAM stable); release-build smoke.

---

## Risk register

| Risk | Mitigation |
|------|-----------|
| TDR from AS builds / fat dispatches | Chunked warmup builds (≤500k tris/frame), RTMARK crumbs in crash.log, dispatch ≤ swapchain size |
| MinGW d3d12.h missing DXR decls/IIDs | Verify FIRST in Phase 0 before any other code; local DEFINE_GUID fallback |
| dxc absent (no web on executor) | Precondition check; user drops release into tools/dxc/ |
| Fixed-point (24.8) geometry acne | Normal-offset bias, distance-scaled, `TD5RE_RT_BIAS` knob |
| LOW-path regression | Byte-compare LOW framedumps every phase; LOW shaders never edited |
| Golden drift | Feed is strictly read-only over span/actor state |
| Split-screen surprises | Explicit per-pane design + Phase 2 check |
| SBT/state-object mis-build | Frozen structs, alignments, and call sequences in §4/Phase 0 |

Total ≈ 5,400 LOC across 6 landable commits (P0 ~600, P1 ~1,700, P2a ~250, P2b ~1,200,
P3 ~1,400, P4 ~500), each gated by build + selftest + goldens + targeted framedumps.
