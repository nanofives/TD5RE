# TD5RE Lighting Rework — Infrastructure Plan (draft v0, 2026-07-02)

Replacement architecture for the current lighting system: physically-plausible
per-pixel lighting, a material-driven reflection system, and ray-cast shadows —
implemented as a **screen-space deferred pipeline** on top of the existing
software T&L, with CPU ray casting reserved for load-time bakes where it wins.

Session: `fix-1783039430-975268-7046`.
Status: **P0 IMPLEMENTED in this worktree** (2026-07-02) — see §11. Delivered:
td5_light2 + td5_material modules, zone-RGB colored vertex lighting (cars, per
original scope), COLOR1 vertex packing (world normal + matid), wrapper G-buffer
MRT (ps_modulate_g / ps_modulate_alpha_g), N.L in the deferred light pass,
`[Lighting] Mode` knob (0 classic / 1 enhance, default 1) + `--LightingMode` +
`TD5RE_LIGHT2_MODE`. Deferred to P1: materials.json editable source, face-normal
derivation for normal-less meshes, G-buffer debug visualizers (logs only for
now). Verified: selftest smoke 12/12, frame drift +0.7%, Bern warm-zone car
R/G 1.067→1.327 with road byte-stable, Moscow/Montego A/B clean.

---

## 1. Diagnosis — why the current look reads "artificial"

Current lighting (all verified in-source, RE basis `ComputeMeshVertexLighting
@ 0x0043DDF0`):

1. **Grayscale luminance only.** Per-vertex `intensity = Σ(3 N·L) + ambient`,
   clamped to `[0x40, 0xFF]`, then pushed through the gray color LUT
   (`i*0x10101`). There is **no light color** anywhere in the base pass — no
   warm sun, no blue sky fill, no dusk tint. (td5_render_mesh.c:268-408)
2. **Floor clamp 0x40** — nothing is ever darker than ~25% white. No real
   shadow tones, flat contrast everywhere.
3. **Headlights (port dynamic lights) have no normals and no occlusion.** The
   deferred pass (ps_light.hlsl) reconstructs world position from depth but
   lights every pixel in range — including surfaces facing *away* from the
   light — and leaks through walls and cars. (ps_light.hlsl:69-100)
4. **No cast shadows** except the car's own ground blob (conforming raycast
   mesh). Buildings, bridges, trees cast nothing; cars never shadow each other.
5. **No reflections.** The RE-faithful chrome overlay exists but is disabled
   (`s_vehicle_reflection_overlay_enabled = 0`, td5_render.c:402); roads never
   reflect anything, wet weather looks dry.
6. **Per-vertex resolution** on large track triangles → faceted light
   gradients.
7. **Authored color is thrown away.** The per-track zone table carries RGB
   directional weights AND RGB ambient per zone (td5_light_zones_table.inc:
   `weight_r/g/b`, `amb_r/g/b` — e.g. warm (128,96,64) sunset zones), but the
   original engine — and the port, faithfully — collapses both to
   `(r+g+b)/3` [CONFIRMED @ 0x42E130 dir, @ 0x43E7B0 ambient]. The color the
   artists authored in 1998 has never been rendered.
8. **Port/original divergence on track meshes.** The original NEVER runtime-
   lights track geometry — track vertex intensity is a baked byte from the DAT
   (`*(u32*)(v+0x18) &= 0xff` at load [CONFIRMED @ 0x40AC00]); only CARS go
   through ComputeMeshVertexLighting. The port synthetically relights
   non-billboard track meshes (td5_render_mesh.c:967), replacing artist bakes
   with the 3-light guess — a likely contributor to the flat look.

## 2. Existing infrastructure we build on (already in-tree)

| Asset | Where | Why it matters |
|---|---|---|
| Per-vertex normals for ALL meshes | `TD5_VertexNormal` array per mesh (td5_types.h:683) | N·L, G-buffer normals, ray bakes |
| Depth SRV + world-pos reconstruction | wrapper `depth_srv`, proven in ps_light.hlsl + soft particles | screen-space rays (shadows, SSR) |
| Fullscreen pass plumbing | `Backend_ApplyLightPass`, vs_fullscreen, additive blend | new resolve/SSR passes are copies of a working pattern |
| **Free 4-byte vertex channel** | `TD5_D3DVertex.specular` — always 0, unused (td5_platform.h:617) | pack world normal (24b) + material id (8b) with ZERO stride/layout change |
| Track lighting zones **with authored RGB** | `td5_light_zones_table.inc` (generated from orig 0x469C78): per-zone dir vector + `weight_r/g/b` + `amb_r/g/b` + blend modes | colored sun + colored ambient per track segment, from ORIGINAL data — no new authoring needed |
| Baked track vertex intensity | DAT mesh data byte at vertex+0x18 (orig never relights track) | authentic albedo-shading base for track geometry |
| Sphere-map math (orig mode 3) | ApplyMeshProjectionEffect @ 0x43DEC0: UV from vertex normal · render-transform + view offset | RE-confirmed envmap fallback formula |
| Surface identity bits | texture-page transparency class (opaque/alpha-test/blend) + STRIP.DAT span surface_attribute byte (+0x01) | material classification seeds |
| Dynamic light registry | td5_light.c — 32 point/spot lights, headlight emitters | reused as-is; new consumer |
| Texture-page types + blend presets | `td5_render_apply_page_blend_preset`, page type 0/2/3 | material classification seed |
| Envmap pages | `ENVMAP_TEXTURE_PAGE_BASE 990` + projection-effect UV modes | reflection fallback when SSR misses |
| Ground down-raycast + wall raycast | td5_track.c:5463 (span/barycentric), td5_camera.c:1679 (2D rails) | existing CPU ray vocabulary |
| Per-pane RenderScratch | td5_render_internal.h:285 | thread-safe per-viewport state home |
| Knob infra | INI `[Lighting]` + `--Key=N` CLI + `TD5RE_*` env + td5_config.h | config surface |
| Profiler + selftest | td5_profile.c, td5_selftest.c degradation gates | perf regression guardrails |

## 3. Architecture decision

Three candidates considered:

- **A. CPU per-vertex ray-traced lighting** — BVH over track meshes, rays per
  vertex per tick. Rejected as the *runtime* path: per-vertex resolution is the
  faceting problem we're escaping, and CPU budget at 30 Hz × 6 actors ×
  split-screen panes is tight. CPU rays are kept for **load-time bakes** (P4).
- **B. Full GPU deferred (albedo G-buffer, all lighting on GPU)** — the clean
  end state, but changes what the color target *means* (albedo vs lit color)
  in one jump; risky against 1999 art baked around the classic look.
- **C. G-buffer-lite screen-space deferred (CHOSEN), staged toward B.** Keep
  the CPU software T&L exactly as-is; add a second render target carrying
  per-pixel **world normal + material id** (fed through the free `specular`
  dword). All new lighting is fullscreen passes that read depth + normal +
  material: colored sun/sky relight, dynamic lights with proper N·L,
  **screen-space ray-marched shadows** (the "ray casting"), and **SSR
  reflections** with material reflectivity + envmap fallback.

Per-viewport frame becomes:

```
opaque world (existing CPU T&L)  → RT0 color, RT1 normal+matid, depth
lighting resolve (fullscreen)    → colored sun N·L, hemisphere ambient,
                                   dynamic lights w/ normals,
                                   ray-marched sun+light contact shadows
reflection pass (fullscreen)     → SSR ray-march vs depth, Fresnel,
                                   material reflectivity, envmap fallback
translucent / VFX / HUD          → unchanged (emissive)
```

Two operating modes, both behind one knob:
- **Mode 1 "Enhance"** (first ship): RT0 keeps the classic lit color; resolve
  modulates it (sun tint, shadow darkening) and adds lights/reflections.
  Low risk; Mode 0 remains byte-identical classic.
- **Mode 2 "Full deferred"** (later): CPU writes albedo-only into RT0; GPU owns
  all lighting. Enabled per-track once the material table matures.

## 4. New modules / files

| File | Contents |
|---|---|
| `td5_material.c/h` | Material table: texture page/class → {albedo scale, specular, roughness, reflectivity, emissive, flags (road/glass/foliage/chrome)}. Data-driven: `re/assets/static/materials.json` (assetsrc pattern) + compiled-in defaults. |
| `td5_light2.c/h` | Scene-light state v2: sun direction+color and hemisphere (sky/ground) ambient derived from the zone table + weather + track; owns the SceneLightCB snapshot per pane. Reuses td5_light.c registry for dynamic lights. |
| wrapper: `gbuffer` RT | `R8G8B8A8` normal+matid target, bound as MRT1 for opaque presets only. |
| wrapper shaders | `ps_resolve.hlsl` (lighting + SS shadows), `ps_ssr.hlsl` (reflections), extended `SceneCB`; scene-color copy for SSR sampling. |
| render glue | Pack world normal (model normal × body rot) + material id into `specular` at T&L; effects/billboards/HUD keep 0 = "emissive" sentinel. |

Build wiring: add new .c files to `srcs.txt` (single source of truth — do NOT
edit the 4 build files). Shaders: fxc step in ddraw_wrapper/build.bat pattern.

## 5. The vertex seam (key trick)

`TD5_D3DVertex.specular` (COLOR1, uint32, always 0 today):
- bits 31..24 → material id (0 = emissive/unlit sentinel: HUD, VFX, billboards)
- bits 23..0  → world normal, 8:8:8 biased (or octahedral 12:12)

Zero stride change, zero input-layout change, zero cost when Mode=0 (still
writes 0). The resolve pass skips matid==0 pixels, so UI/VFX are untouched.

Precedent: `ps_fx_smoke.hlsl` already smuggles particle age/seed through this
same COLOR1 channel — per-preset reinterpretation of the dword is proven
in-tree. Smoke draws are translucent (never G-buffer-bound), no conflict.

**Normals caveat (P0 work item):** normals are a separate 16-byte/entry stream
(mesh+0x34), guaranteed for cars but per-mesh optional for track geometry
[research: presence varies; not audited against assets]. Mesh-prepare
(td5_asset.c) must derive face normals from triangle winding for any mesh with
`normals_offset == 0` — roads are the #1 reflective/shadowed surface, so this
is mandatory infrastructure, not polish.

## 6. Shadows (ray casting)

- **Screen-space contact shadows**: per pixel, march 8–16 steps along the sun
  (and per-dynamic-light) direction in view space against the depth buffer
  (reconstruct math already proven in ps_light.hlsl). Darken the direct term on
  hit. Gives car-on-road, car-on-car, wall/building-on-road shadows for
  anything on screen. Documented limit: off-screen occluders can't cast.
- **Keep** the conforming raycast ground shadow (it anchors the car visually
  even when the sun term is ambiguous); retire the double-darkening once SS
  shadows land (blend knob).
- **P4 option**: load-time CPU BVH over track meshes → bake per-vertex static
  sun visibility + AO into mesh data (millions of rays, once, threaded at load)
  → persistent building shadows independent of camera. This is where "simple
  ray tracing" pays off without a runtime budget.

## 7. Reflections

- **SSR**: march the reflected ray (about the G-buffer normal) through the
  depth buffer; on hit, sample the pre-pass scene-color copy; fade by ray
  travel + edge proximity. Weight = material reflectivity × Fresnel(N·V).
- **Materials**: road (reflectivity scales with rain/wet weather state from
  vfx), car paint (moderate, roughness from material), glass/windows (high),
  chrome trim (very high). All from td5_material table.
- **Fallback**: on SSR miss → sample envmap pages 990+ (existing asset) or
  procedural sky gradient, so reflections never "cut out".
- The disabled RE-faithful vehicle chrome overlay stays as the Mode-0 relic;
  SSR supersedes it in Mode ≥1.

## 8. Config surface

`[Lighting]` additions (all with `--Key=N` CLI + `TD5RE_*` env A/B):

```
Mode         = 0 classic | 1 enhance (default target) | 2 full deferred
SunShadows   = 1        ShadowSteps = 12
Reflections  = 1        SSRSteps    = 24
WetRoads     = 1 (auto from weather)
MaterialDebug= 0 (1..4 = view normals / matid / shadow mask / SSR mask)
```

F12-style in-race debug cycle for the visualizers (dev builds only).

## 9. Determinism & isolation rules (non-negotiable)

- Render-only: never touches sim state and **never consumes the shared MSVC
  rand stream** (td5_msvc_rand — netplay/replay determinism). Any jitter =
  hash(pixel, frame).
- `Mode=0` stays byte-identical to today (trace parity; /diff-race unaffected).
- All new per-pane state lives in RenderScratch (threaded pane safety); GPU
  passes stay per-viewport like `td5_render_apply_light_pass` today.
- G-buffer RT1 bound ONLY for opaque presets — the blend-preset hook
  (`td5_render_apply_page_blend_preset`) is the single choke point.

## 10. Performance budget

- GPU: +1 RT write in opaque pass (~free), 2 fullscreen passes ≈ 1–3 ms @1080p
  on modest hardware; per-pane in split-screen → scale step counts by pane
  count (knob).
- CPU: normal packing is a few ops per vertex inside the existing lighting
  loop (which already touches every vertex). Load-time bake (P4) is threaded
  and one-shot.
- td5_profile phase counters for resolve/SSR; selftest full-suite degradation
  gates (frame-time) are the regression tripwire.

## 11. Phases

| Phase | Deliverable | Visible result |
|---|---|---|
| **P0 Infrastructure** | td5_material + td5_light2 modules, materials.json, vertex packing, load-time face-normal derivation for normal-less meshes, zone-RGB plumbing (stop averaging `weight_r/g/b`), wrapper MRT + SceneCB + pass skeletons, knobs, debug visualizers, srcs.txt wiring, docs | Mode 1 minimal: authored zone COLOR resurrected (colored sun/ambient) + N·L dynamic lights (headlights stop leaking) |
| **P1 Lighting** | Full sun/sky hemisphere model per zone/weather, tone curve, dark floor removal in new mode | The actual "less artificial" relight |
| **P2 Shadows** | SS contact shadows (sun + dynamic lights), conforming-shadow blend | Cars/walls cast real shadows |
| **P3 Reflections** | Scene-color copy, SSR, material reflectivity, wet-road weather hook, envmap fallback | Wet roads, glossy car paint |
| **P4 Bakes (optional)** | CPU BVH over track meshes, load-time per-vertex sun-visibility + AO bake | Persistent building shadows, grounded corners |

Each phase = its own /fix worktree + A/B knob; user confirms look per phase.

## 12. Risks / open questions

1. **Style drift**: smooth GPU lighting on 1999 art may clash — mitigation:
   tone-curve + optional quantize/dither knob to keep the period feel.
   (User taste gate per phase.)
2. **Deferred-additive street lights** draw during the world pass with additive
   blend — must not write RT1 (handled by preset-scoped MRT binding).
3. **TD6 baked vertex ARGB** becomes albedo in Mode 2 — expected fine (it is
   albedo-ish), needs a look check.
4. **specular-dword reuse**: audit every shader consuming COLOR1 (composite/
   modulate ignore it today) before repurposing; Mode 0 always writes 0.
5. **Split-screen cost** with 4+ panes — step-count scaling knob.
6. SSR artifacts at screen edges / occlusion gaps — standard fade heuristics,
   envmap fallback hides the worst.

## 13. RE findings (original binary) — research agent, 2026-07-02

Method: headless Ghidra decomp dump over pool slot TD5_pool0 (read-only).
All items [CONFIRMED @ addr] unless tagged.

**Vertex lighting (CARS ONLY):**
`intensity = Σ dot(normal, L0..L2) + ambient`, rounded, clamped [0x40,0xFF],
stored at vertex+0x18 [0x43DDF0]. Light dirs = 9 floats @ 0x4AB0D0 written by
UpdateActiveTrackLightDirections @ 0x42CE90 (zone dir rotated into model space
by actor rotation, per-light enables). Ambient int @ 0x4BF6A8 =
`(amb_r+amb_g+amb_b)/3` [ComputeAverageDepth @ 0x43E7B0]. Directional
contribution = zone dir_shorts × `((weight_r+weight_g+weight_b)/3)` ×
`_k_lightContribDirScale` [SetTrackLightDirectionContribution @ 0x42E130].
→ **Both RGB triples in the zone data are averaged to gray at runtime.**

**Track/sky meshes are NOT runtime-lit:** track path =
TransformAndQueueTranslucentMesh @ 0x43DCB0 (transform only); vertex+0x18 is a
baked byte, masked `&0xFF` at load [PrepareMeshResource @ 0x40AC00].

**Zone select/blend:** per-track 0x24-byte zone records
(`_g_trackLightingZoneTablePtr`, table @ 0x469C78, bound in LoadTrackRuntimeData
@ 0x42FB90); blend modes 0=static / 1=transition / 2=multi-sample / 3=half-blend
sample STRIP.DAT verts [ApplyTrackLightingForVehicleSegment @ 0x430150].

**Vertex layout:** 0x2C stride: +0 pos.xyz f32, +0xC view.xyz (written by
TransformMeshVerticesToView @ 0x43DD60), +0x18 intensity/ARGB, +0x1C/+0x20 UV.
**Normals = separate stream** at mesh+0x34, 16 B/entry (nx,ny,nz f32 +
flag@+0xC), 1:1 per expanded vertex; flag = "all corner-normal Y >
_DAT_0045D618" per primitive [0x40AC00], gates planar UV projection.

**Draw color:** at flush, intensity byte → 256-entry LUT @ 0x4AEE68 =
`0xFF000000 + i*0x10101` — an **opaque grayscale ramp**
[InitializeTranslucentPrimitivePipeline @ 0x4312E0; applied @ 0x4314B0].
FVF 0x1C4, DrawPrimitive.

**Fog:** D3D linear table fog (renderstate 0x23=3), color = d3d_exref+0x18,
start/end/density = +0x20/+0x24/+0x28, enable +0x1C [ApplyRaceFogRenderState
@ 0x40AF50]. Per-track enable gate LEVELINF+0x5C, color LEVELINF+0x60..0x62
[InitializeRaceSession @ 0x42AA10]. [UNCERTAIN] start/end/density writers not
found in exe — set inside M2DX; port uses 0.60/1.00/0.40 defaults.

**Reflection (original):** player-slot-0 only — second mesh resource
`g_playerReflectionMeshResource` rendered when `g_vehicleProjectionEffectMode
== 2` [RenderRaceActorForView @ 0x40C120]. ApplyMeshProjectionEffect @
0x43DEC0 modes: 1 = planar Y UV; 2 = rotated planar (sin/cos @ 0x4BF528/2C);
3 = **sphere-map**: UV from vertex normal · render-transform rows + view-pos
offset. Textures = environs.zip `ENV%d` pages; final page = per-effect page +
0x400 [LoadEnvironmentTexturePages @ 0x42F990]. [UNCERTAIN] per-zone effect
param configuration (ConfigureActorProjectionEffect @ 0x40CBD0 not decompiled).

**Shadows (original):** two multiplicative textured quads ("shadow" texture,
color 0xFFFFFFFF) per vehicle, corners = wheel contact probes, Y offset −22.0
(replay −4.0) [0x40BB70, 0x40C120, 0x40C7E0]. No projected geometry, no other
shadow mechanism.

**Light sources (original):** taillights = 2 fixed-color 0xFF909090 glow quads
at car-def hardpoints +0x60/+0x68, brake byte ramps +8 [0x4011C0] — sprites
only, no light emission. **No headlights. No D3D lights anywhere** (no
SetLight/LightEnable in any decompiled function).

**Material identity:** per-command texture_page_id + transparency type
(opaque/alpha-test/blend) via GetTextureSlotStatus [0x40AC00]; normal
flag +0xC; STRIP.DAT span surface_attribute byte (+0x01).

**Unknowns (explicit):** fog start/end/density origin (M2DX internals);
ConfigureActorProjectionEffect @ 0x40CBD0 internals; PatchModelUVCoords-
ForTrackLighting @ 0x443730 caller/purpose; brake-brightness byte consumer;
envmodel.dat reflection-mesh loader; whether any original track mesh ships a
normals stream (not audited against assets — P0 derives face normals as
fallback regardless).
