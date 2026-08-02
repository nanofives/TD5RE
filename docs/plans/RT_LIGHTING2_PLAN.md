# RT_LIGHTING2_PLAN — Sun & sky system, full-scene shadows, GI, material detection, per-feature options

Authored 2026-08-01 (Fable 5 session, account3). Executor: a separate Claude Code session
(account2 / Accenture-managed — per-call approvals, **no web access**, no Ghidra/frida MCP,
no Workflows). This is the follow-up to `RT_LIGHTING_PLAN.md` (all phases 0–4 shipped on
branch `rt-lighting`); **read that plan's as-built notes before starting** — they contain
the frozen SBT/RS layouts, the alignment lessons, and every gotcha already paid for.
This document is self-contained for everything NEW; it does not re-freeze what the first
plan froze — those decisions stand.

Execution model: **one phase per session/loop iteration**, in order, each phase = one
commit on branch `rt-lighting2` (branched from `rt-lighting`). The user runs these in a
loop, one by one — a phase must leave the tree shippable (gates green) even if the loop
stops there.

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Sky probe: sun detection, sunny/overcast classing, sun disc + authored sun dir | ✅ done (as-built below) |
| 2 | Full-scene RT geometry feed (scenery + cutout billboards into the TLAS) | ✅ done (as-built below) |
| 3 | Unified shadow treatment (billboards/translucents receive; everything casts) | ✅ done (flat-billboard receive deferred — see as-built) |
| 4 | RT sky-visibility GI + baked/zone darkening replacement in HIGH | ⬜ not started |
| 5 | Material shininess detection (texture-analysis classifier, per-page table) | ⬜ not started |
| 6 | Reflection & shadow range/angle fix + precision knobs | ⬜ not started |
| 7 | HIGH light pipeline: headlights, street lights, sun as realistic RT lights | ⬜ not started |
| 8 | LIGHTING OPTIONS screen: per-feature rows, INI, defaults, release gates | ⬜ not started |

Append an **as-built note** under this table after each phase (deviations, measurements,
gotchas found) — exactly like RT_LIGHTING_PLAN.md does.

### As-built — Phase 3 (2026-08-02, branch `rt-lighting2`)

**Delivered**: unified shadow RECEIVING for the two classes the opaque sun-shadow
composite misses. Casting is already universal (P2 put the whole world in the
TLAS). Concretely: (1) **alpha-blend world translucents** (SRCALPHA_INVSRC,
depth-tested) now sample the sunvis mask at draw time via new PS variants and
darken exactly as the opaque composite does; (2) the **opaque world** — road,
cars, and 3D-mesh scenery incl. tree meshes — already receives building/bridge
shadows through the existing composite now that P2 feeds the TLAS (verified: the
Australia-style "building shadow on the road/car" is the P2+composite path).

**Translucent receive (the built path)**: `PS_MODULATE_SHADOWED` /
`PS_MODULATE_ALPHA_SHADOWED` (ps_modulate_shadowed.hlsl/_alpha, `_50` loop in
compile_shaders.bat, `PS_COUNT`=8). The main root sig `s_root_sig` grew param[5]
= a `t1` SRV table for the sunvis mask; `s_srv_ring` slot 0 is RESERVED for it
(ring cycles [1..cap)); `d3d12_dxr_sunvis_ready()/_resource()` expose the mask
(the shadow pass leaves it in `PIXEL_SHADER_RESOURCE`, so no extra transition).
The `recv_shadow` gate in `d3d12_bind_and_draw` (HIGH + SRCALPHA_INVSRC + z-test +
sunvis-ready) creates the sunvis SRV into ring slot 0, selects the shadowed PS,
and binds param 5. VERIFIED with a magenta debug: a real translucent (the car
underglow) receives the shadow. LOW is byte-identical — the PS/gate all gate on
`s_rt_mode`; the extra root-sig param is inert for LOW (goldens match on all 4
golden races, smoke 15/15). Env A/B: `TD5RE_RT_TRANSLUCENT_SHADOW`.

**DEFERRED with rationale — flat opaque cutout BILLBOARDS** (the camera-facing
tree/sign SPRITES, opcode 4 → `dispatch_billboard`): these are page-type-1
color-keyed OPAQUE (not translucent), queued into the projected depth-sort
buckets and drawn LATER. The blocker is the **parallel rcmd record/replay**
architecture: the bucket flush runs on a worker while RECORDING a pane list, so
the actual `d3d12_bind_and_draw` happens at REPLAY on the main thread —
decoupled from any live phase flag (diagnostic-confirmed: a `translucent-phase`
flag set during the flush is never active at the draw). Two attempts were made
and reverted: (a) a live phase flag around the flush (never active at replay);
(b) recording the phase into the rcmd command stream (`RC_RT_PHASE`) — this fired
but produced a heavy per-draw G-buffer-target thrash. Both removed cleanly. Since
the 3D-mesh foliage (the majority) ALREADY receives via the composite, and flat
sprite billboards are the minority + thin, this is a documented follow-up (the
clean fix is to record the phase marker into rcmd AND skip the dead post-composite
G-buffer write without toggling render targets per draw). The translucent path +
composite cover the substantive decision-#3 payoff.

**GOTCHAs paid**: `td5_render_flush_translucent()` is a DEAD no-op
(`s_translucent_head` always -1). Billboards use `td5_render_flush_projected_
buckets` (via `td5_render_queue_projected_entry`), which routes through
`clip_and_submit_polygon`. There are THREE render branches (single-pane else at
game.c ~7320 + parallel rcmd at ~7200 + a photobooth/minimap path at ~5750); the
ACTIVE 1-player path is the parallel rcmd one. The G-buffer normal pack format is
`specular = (matid<<24) | (bx<<16|by<<8|bz)` biased Y-flipped (td5_render.c:1324),
`TD5_MAT_CUTOUT`=2 — documented for the follow-up.

**Gate**: build_all clean (dev+release, lint OK, no new warnings — the flagged
td5_platform_win32.c warnings are pre-existing baseline); goldens match on all 4
golden races (LOW byte-identical); smoke 15/15. NOTE: this session's GPU was
thermally throttled after hours of RT-HIGH testing — HIGH FPS read ~12 (fresh-GPU
A/B earlier this session was 125–134 on Courmayeur) and the full-suite
`degrade-private-bytes` came in at +25.2 MB (limit 24) — the DOCUMENTED variable
straddle (4–28 MB), pre-existing (P3 allocates nothing in LOW; goldens match), not
a regression. Re-verify perf/degrade on a cooled GPU.

### As-built — Phase 2 (2026-08-01, branch `rt-lighting2`)

**Delivered**: the TLAS is now the whole visible world. Every MODELS.DAT display-
list scenery mesh (buildings/walls/bridges/terrain) is a static world BLAS, and
billboard-tag meshes (trees/signs) are cutout crossed quads; `anyhit_cutout` is
activated end-to-end. TLAS instance cap 128 -> 2048. Verified on Moscow (the
heaviest track): **1610 scenery meshes fed** (885 solid + 725 billboards), 0
dropped, 0 pool overflow, renders at **89 FPS** (Courmayeur 128 FPS); no crash,
no TDR across ~7 HIGH races + the device-lost drill.

**GEOMETRY-SOURCE ODYSSEY (the hard part — 5 rebuild/GPU cycles).** Getting the
world geometry required peeling several layers; recorded so the next feed change
doesn't repay it:
1. `td5_track_get_models_display_list_count()` is **TD6-only** (returns 0 on TD5
   tracks like Moscow) — do not bound a TD5 enumeration by it.
2. `td5_track_get_display_list(span)` returns **STRIP road blocks** whose
   per-command `vertex_data_ptr` is a **truncated x64 pointer** (uint32_t on-disk
   field) the renderer itself skips — NOT the geometry. The real buildings come
   from `td5_track_get_display_list_entry(entry_idx)` over `[0, (ring+3)>>2)`
   (`ring = td5_track_get_ring_length()`), the renderer's main-walk source.
3. Scenery meshes are **authored in WORLD space** (drawn identity) so they feed
   like the road lanes (identity instance) — BUT `rt_build_actor_mesh`'s
   sequential-vertex assumption fails: their commands carry **per-command vertex
   bases** in the blob. New `rt_build_scenery_mesh` collects each command's
   vertices (validated via `td5_track_is_ptr_in_blob` — a truncated ptr that
   passes a looser `is_valid_mesh_ptr` check faulted at 0xC0000005; blob-bounds
   only, matching the renderer) and indexes into its own buffer. Topology is
   `tri_count` tris + `quad_count` quads per command for EVERY opcode (opcode =
   render state, not topology).
4. **TIMING (the final blocker)**: the `td5_rt_level_build` hook runs from the
   track loader BEFORE MODELS.DAT is parsed, and the lazy re-feed is skipped once
   the road lane-quads set `s_track_chunk_count>0`. So the scenery feed is now
   done from `td5_rt_frame` on the first HIGH frame (`s_scenery_fed` one-shot,
   reset on device-lost/unload), by which point InitRace step 7 has built the
   display lists.

**Cutout**: `matid_flags` bit `0x100` (DXR_MATID_CUTOUT / RT_MATID_CUTOUT) flags
alpha-test ranges; the wrapper builds those BLAS geometries NON-opaque so the new
`anyhit_cutout` runs (samples the bindless page alpha at the barycentric UV,
`IgnoreHit()` < 0.5). Everything else stays OPAQUE (fast early-accept, esp. shadow
rays). **DXR gotcha**: a hit group has ONE any-hit but shadow rays use
`ShadowPayload` and reflection rays `RayPayload` — `anyhit_cutout` declares
`RayPayload` but never touches `p` (only IgnoreHit/accept), so the payload is
inert for both and the mismatch is harmless (CreateStateObject succeeds, pipeline
inits OK, verified). Shadow rays keep `SKIP_CLOSEST_HIT_SHADER` but do NOT
force-opaque, so cutout billboards cast alpha-shaped shadows.

**Budget/caps**: `DXR_MAX_INSTANCES` 128->2048 and `DXR_MAX_MESHES` 512->2048
(deviation from the plan's 1024 — Moscow alone is 1610 scenery meshes; the
instance-desc/GeoRecord/DxrMesh arrays scale but are KB-scale, TLAS build stays
sub-ms). Game-side `RT_MAX_SCENERY=1900`. Moscow feed fits the existing 128 MB VB
/ 48 MB IB pools with NO overflow, so pools were NOT grown. One BLAS per mesh
(GeoRecord is per-BLAS: one texture + matid), identity-instanced.

**Feed table** (TD5RE_RT_DIAG): Moscow = 698 display-list entries -> 1610 unique
meshes (885 solid + 725 billboard) -> 1610 BLAS + 2 road chunks + ~12 actors ≈
1624 TLAS instances. Perf: normal HIGH race 89 FPS (Moscow) / 128 FPS
(Courmayeur) vs pre-P2 ~126 / ~145 — the full-scene shadow/refl cost, still very
playable. Knobs: `TD5RE_RT_SCENERY` (default 1), `TD5RE_RT_BILLBOARDS` (default 1),
`TD5RE_RT_DIAG` (feed stats).

**Files**: `d3d12_dxr.c` (caps, anyhit export + hit group, CUTOUT geo flag),
`rt_pipeline.hlsl` (anyhit_cutout body), `td5_rt.c` (scenery+billboard feed,
rt_build_scenery_mesh, frame-time gating).

**Gate**: build_all clean (dev+release, lint 3/3, no new warnings); full selftest
golden hashes match on all 4 golden races, degrade-private-bytes PASS (LOW feeds
nothing — gated on td5_rt_active); smoke 15/15 clean; DXR pipeline inits OK with
the anyhit state object; device-lost drill: process survives + scenery **re-fed**
(handles=1610) + pipeline re-inits, no crash; Moscow/Courmayeur HIGH framedumps
show the full world rendering correctly (no corruption). The full-suite trailing
0xC0000005 is the documented intermittent race-moscow env-GPU TDR (smoke clean +
LOW byte-identical => not a regression).

**OWED (long/finicky, not implementation)**: the 10-min Moscow soak (spot-checked:
~7 clean HIGH races incl. device-lost, no TDR/VRAM growth observed, but the
literal 10-min run is owed); a `TD5RE_RT_DEBUGVIEW` framedump that lands mid-race
(AutoRace cycles race->results->car-select so the single-frame dump kept catching
the car-select splash; the RASTER race render was verified correct instead — same
TLAS). **Deferrals with rationale**: meshes >65535 verts are skipped (u16 index
limit; `big_skipped` — none on Moscow); per-mesh single texture page (GeoRecord is
per-BLAS — a mixed-page mesh reflects its header page, as the actor feed already
does); billboards use bounding-centre/radius crossed quads (not per-leaf).

### As-built — Phase 1 (2026-08-01, branch `rt-lighting2`)

**Delivered**: image sky probe (classify NIGHT/SUNNY/OVERCAST + image-derived sun
dir), sun-source override for the shadow + SSR passes (HIGH), overcast soft-shadow
treatment (wide cone + strength cap), and a sun disc. Offline tuning harness
`re/tools/sky_probe.py` + the classification table (the phase's core deliverable).

**The classification table (the deliverable)** — thresholds `ratio>=1.8 peak>=200
night<80 area<0.20`, sky-band `HORIZON=0.55`, `SAT>=0.18`, `BLUE>=0.55`; luma
`(R+G+B)/3` box-downsampled to <=128. Run `python re/tools/sky_probe.py`
(CSV in `log/sky_probe_table.csv`). Distribution over all 31 forward skies:
**18 SUNNY / 8 OVERCAST / 5 NIGHT**. In-engine probe matches the offline tool
exactly (verified: Maui=SUNNY, Courmayeur=SUNNY, Edinburgh=OVERCAST, Moscow=NIGHT).
Spot-checked skies by eye: the OVERCAST set (001/006/007/008/009/012/016/029) are all
genuinely cloud-dominated/hazy (soft-shadow-appropriate); the SAT gate correctly
reserves SUNNY for saturated deep-blue skies. NIGHT = 005/018/023/030/037 (dark or
black-sky). Night tracks with a bright moon/horizon (023 ratio 3.6) still classify
NIGHT via the mean gate.

**CORE FINDING — TD5 sky panoramas have no resolvable sun disc.** The brightest
sky-band region is almost always the **horizon glow band** (atmospheric scatter) or
cloud highlights, not an elevated sun; the panoramas also bake the ground/water into
the lower half (level002's naive "sun" was the bay at v=0.80). So: (a) the peak/
centroid search is **restricted to the sky band** (top 55%); (b) brightness+contrast
alone can't separate clear-blue-sunny from grey-overcast, so a **blue-saturation
test** was added (deviation from plan §1.1, which assumed a locatable disc); (c) the
image gives a reliable **azimuth** (bright side of the sky) but an unreliable
**elevation** (usually resolves to the horizon). Decision: keep the centroid azimuth,
**clamp elevation to a low sun** (`TD5RE_SUN_MIN_ELEV` default 0.15 ~8.6deg) so every
SUNNY track gets a grounded directional sun + long dramatic shadows. Genuinely high
"tight" suns (a real bright break, e.g. Maui at ~33deg) keep their elevation.

**Sun-dir convention (the sign the plan warned about)** — the `.prr` dome is authored
**+Y-UP** (verified in-engine: a v~0.22 sky UV maps to a dome vertex with pos_y=+0.55),
same convention as the zone light dirs, while the shadow march is +Y-DOWN position
space. So the dome-vertex position needs the **same Y-flip** the zone sun gets
(`dir=(px,-py,pz)`) — NOT the raw position. Also resolved the plan's dome-rotation
worry: the `.prr` dome draw uses identity model rotation (camera basis only); it does
**not** apply `s_sky_rotation_angle`, so model X/Z == world X/Z and there is no disc/
shadow drift — no rotation composition needed.

**Sun disc** — implemented self-contained (the headlamp-glow recipe: additive-glow
preset + 1x1 white page 899 fallback, own `fx_begin/end`), drawn **after** the opaque
world + deferred passes (game.c, before translucent VFX) at far depth 0.999 so scene
geometry z-occludes it and it shows only over open sky. **GOTCHA paid**: the first
attempt reused `arcade_emit_glow_at` which passes `tex_page=-1` and only draws when
proc-FX glow fires — it silently drew nothing; the self-contained quad fixed it
(verified via `TD5RE_SUN_DISC_DBG=1`, which aims it close+centred: a clear bright
glare renders). **Framing reality**: the sun is a physically-correct world sprite, so
it's only visible when the camera looks toward it over open sky — in a chase cam it's
frequently off-frame-top (high sun) or building-occluded (low sun off-axis). That is
correct behaviour, not a bug; the disc is confirmed to render + occlude properly.

**Overcast** — keeps the zone sun (artist intent) but widens the RT shadow cone via
new ShadowCB field `params2.w` (`rgen_shadow` cone spread `0.012 * coneScale`, was
hardcoded) and caps strength ~0.5. Knobs `TD5RE_SUN_OVERCAST_CONE`(5)/`_STRENGTH`(0.5).
RT-only: the LOW screen-space shadow shader reads only `params2.x`, so LOW is
byte-identical (verified: goldens match, degrade +1.7 MB flat).

**Unified helper** — `td5_render_scene_sun()` now derives the sun for BOTH the shadow
and SSR passes (was duplicated), applying the probe override in HIGH. Classes: 0=none
(tunnel), 1=SUNNY (crisp), 2=OVERCAST (soft). NIGHT/probe-off/LOW keep the zone sun.

**DEFERRED (with rationale)**: plan §1.4's "sun tint into `miss_refl` via SSRCB" (RT
sky-reflection glint) — secondary "Also" item; needs an SSRCB struct field + HLSL
mirror. Skipped to keep the diff focused; the sun tint IS captured (`s_sky_sun_rgb`,
feeds the disc colour) so wiring it later is a small CB add. Noted for a follow-up.

**Knobs added** (all inline getenv, RT-code style): `TD5RE_SUN_PROBE`(1),
`TD5RE_SUN_RATIO`(1.8), `TD5RE_SUN_PEAK`(200), `TD5RE_SUN_MIN_ELEV`(0.15),
`TD5RE_SUN_OVERCAST_CONE`(5), `TD5RE_SUN_OVERCAST_STRENGTH`(0.5), `TD5RE_SUN_DISC`(1),
`TD5RE_SUN_DISC_SIZE`(0.03), `TD5RE_SUN_DISC_DBG`(0). Phase 8 surfaces the durable ones.

**Files**: `re/tools/sky_probe.py` (new), `td5_render.h` (accessor + enum + disc decl),
`td5_render_effects.c` (probe/classify/resolve/accessor/disc), `td5_render_mesh.c`
(unified scene-sun helper + overcast in shadow/SSR passes), `td5_platform.c/.h`
(apply_shadow `cone_scale` param), `rt_pipeline.hlsl` + `rt_common.hlsli` (params2.w
cone scale), `td5_game.c` (disc draw site).

**Gate**: build_all clean (dev+release, lint 3/3, no new warnings — the one flagged
misleading-indentation is the pre-existing s_rays clamp, untouched); full selftest
**54 PASS / 4 WARN / 0 FAIL**, all 7-module golden hashes match on Moscow/Pelton/drag/
split, degrade-private-bytes flat; smoke 15/15 clean. The full-suite trailing
`0xC0000005` is the documented intermittent race-moscow env-GPU TDR (LOW is
byte-identical + smoke clean — not a regression). Framedumps at HIGH (126-146 FPS, no
crash, RT active): SUNNY disc primitive proven (DBG) + shadows use the probe sun,
OVERCAST no disc + soft, NIGHT unchanged. Perf unchanged (probe is load-time; the
override is a per-pane branch).

## 1. Goal & user decisions (locked — do not revisit)

1. **Sun on sunny maps, derived from the skybox image.** At sky-load time, analyze the
   sky texture: locate the sun (brightest coherent region) and classify the sky
   (sunny / overcast). Sunny → place a fixed, per-track sun (visible disc + authoritative
   light direction inferred from the image, including a noon-vs-afternoon elevation read).
   Overcast → **no visible sun, but shadows still exist** (soft, low-contrast directional).
2. **Global illumination (sky-visibility) term**: outdoor areas read brighter than
   covered/indoor areas (under bridges, tunnels, canyons) — computed with rays in HIGH.
3. **Uniform shadow treatment**: every screen element — cars, road, scenery, and 2D
   billboards (trees/signs) — receives AND casts shadows the same way. No element class
   is exempt.
4. **Fix the legacy near-distance / camera-angle limitation** of reflections and shadows
   (the screen-space marches only resolved close geometry and broke with camera angle).
   In HIGH this must be range-complete and view-independent.
5. **Material shininess detection**: a detection phase that inspects textures and infers
   shininess/reflectivity per page (car chassis, car/building windows, scenery glass,
   water) — replacing the coarse 6-class transparency-based mapping for reflectivity.
6. **Per-feature graphics options**: shadow quality/resolution, reflection precision,
   GI quality, sun system, light quality — each independently tweakable, **all default
   to highest**. LOW remains the single switch that bypasses all of it.
7. **Baked/analytic darkening on cars** (e.g. the darkened cars under the bridge at the
   start of Australia) is LOW-only. In HIGH, that darkening comes from raycasts (the GI
   term of #2), not from zone dark-mode or baked shading.
8. **Headlights**: LOW keeps the current system untouched. HIGH renders headlights, sun
   and street lights realistically through the RT light pipeline (the existing emitter
   registry stays the single source of light definitions).
9. LOW quality (`[Lighting] Quality=0`) stays behaviorally byte-identical to current
   `rt-lighting` tip in every phase. Golden traces stay green in every phase (RT is
   render-only).

## 2. As-built survey facts (verified 2026-08-01 — trust these, spot-check line numbers)

### RT system (branch `rt-lighting`, all phases shipped)

- **`d3d12_dxr.c`** (~1,520 LOC): BLAS/TLAS build, pooled DEFAULT VB/IB (128/48 MB,
  bump-allocated), chunked BLAS (≤500k tris/frame) in `Backend_RTSceneEnd`,
  double-buffered TLAS (**cap 128 instances**, PREFER_FAST_BUILD), state object, SBT
  (frozen `[raygen|miss|hitgroup]`, raygen 64-strided, miss/hit 32), global RS
  (b0–b3 root CBVs + fixed table UAV u0–u3 / SRV t0–t5 + **separate unbounded bindless
  range `t0,space1`** at heap slot 16), 4096-slot heap ([0..15] fixed, [16..16+1024)
  bindless page textures), passes `d3d12_dxr_shadow_pass/_light_pass/_ssr_pass` via
  shared `dxr_lighting_pass`, `dxr_make_composite_pso` (composites preserve dest alpha —
  the framedump-alpha lesson), `Backend_RTRegisterBoundPage`, `d3d12_dxr_shutdown` +
  generation counter for device-lost re-feed.
- **`rt_pipeline.hlsl`** (255 LOC, one `lib_6_3` DXIL blob): `rgen_smoke`, `rgen_debug`,
  `rgen_shadow` (K stratified cone samples, K=`TD5RE_RT_RAYS` default 4, bias
  `TD5RE_RT_BIAS`), `rgen_light` (attenuation+cone+soft-wrap Lambert + shadow rays,
  `RT_LIGHT_MAX=32`), `rgen_refl` (matid→reflectivity LUT + wet boost + Fresnel gate),
  `chit_refl` (u16 index unpack, barycentric UV, bindless texture sample, geo-normal
  shading, depth-2 sun shadow ray), `miss_shadow`, `miss_refl` (**flat constant
  `float3(0.02,0.02,0.12)` sky** — no real sky), hit group `"hg"` = chit_refl +
  `anyhit_cutout` (**declared but dead** — no cutout geometry is fed).
  `MaxTraceRecursionDepth=2`. Helpers in `rt_common.hlsli`.
- **Game feed `td5_rt.c`** (416 LOC): `td5_rt_level_build` walks the span table emitting
  **per-lane road quads only** (`td5_track_get_lane_quad_world`, extent>12000 rejected);
  `td5_rt_frame` instances **actor vehicle meshes only** (`rt_build_actor_mesh` cache).
  **No world scenery, no props, no billboards are fed** (deliberate — the feed-in-LOW
  memory regression; feed is now gated on `td5_rt_active()`).
- Activation: `td5_rt_active() = available && quality==HIGH && [Lighting] Enabled`;
  INI `[Lighting] Quality` (default 1), `--Quality=N`, dev env `TD5RE_RT=0|1` A/B
  override, `TD5RE_RT_DISABLE=1` kill-switch. Menu row 6 on `Screen_DisplayOptions`.
- Perf reference: HIGH normal race ≈ **172 FPS** with 4 shadow samples on the 5070 Ti.
  Memory budget from plan 1: total RT < 500 MB uncompacted.

### Sun & sky today

- Sun dir is **derived per-pane from the light-zone table**, not authored: shadow pass
  (`td5_render_apply_shadow_pass`, `td5_render_mesh.c:225-281`) and SSR pass pick the
  strongest enabled `s_tl_contrib[s].vec_world` directional slot, normalize, **flip Y**
  (zone dirs are in the original's Y-flipped lighting convention; rays march in position
  space, world +Y down). Rejected if `best_mag2 <= 1.0` or `sun[1] >= -0.05` (below
  horizon). The same sun feeds RT via ShadowCB `sh_sun` and SSRCB `sr_sun`.
- Sky: `td5_render_load_sky(path)` (`td5_render.c:3208`), fixed page
  `SKY_TEXTURE_PAGE 1020`, dome mesh `s_sky_mesh` (`sky.prr`), uploaded via
  `td5_plat_render_upload_texture`. **The engine already probes the sky texture at load
  time**: average luminance → `s_sky_luma` (`td5_render_sky_luma()`), used as the
  auto-headlight brightness baseline. Extend this probe; don't build a second one.
- Sky dome **rotates at runtime**: `s_sky_rotation_angle` (12-bit angle, incremented
  per tick), drawn in `td5_render_effects.c`; TD6 dome pitch `TD6_SKY_PITCH_DEFAULT
  0.12f` (`TD5RE_SKY_PITCH`). Any image-derived sun azimuth must be composed with the
  current dome rotation or the disc and the shadows will drift apart.
- Sky is not in the TLAS; RT reflections of sky are the flat `miss_refl` constant.

### Baked / analytic darkening (the thing #7 replaces in HIGH)

In `td5_render_mesh.c` (~L585–715):
- **Cars (slot≥0)**: synthetic per-vertex 3-directional diffuse (original 0x43DDF0):
  `intensity = dot(N,l0)+dot(N,l1)+dot(N,l2)` clamped `[0x40,0xFF]`, written at vertex
  +0x18; dirs `s_light_dirs[0..8]`, plus `s_ambient_intensity`, paint tint, damage scuff.
- **Zone dimming** (what actually darkens cars under the Australia bridge):
  `s_light_dark_mode` + `s_dark_scale` (0.50) + `s_dark_floor` dim ambient+directional
  per light zone; auto-dark probe `td5_render_env_is_dark_for_slot`
  (`td5_render_mesh.c:380-438`) — also the auto-headlight trigger.
- **Track/scenery (slot<0)**: TD6 artist-baked per-vertex grey preserved as full ARGB
  diffuse (`prelit`, `TD5RE_TD6_VLIGHT` default on). This is authored art content.
- LIGHT2 zone chroma: `s_tl_chroma`, `s_tl_amb_rgb`.

### Billboards / 2D sprites

- Drawn via retained-mesh opcode 4 (`InsertBillboardIntoDepthSortBuckets`, 0x43E3B0) →
  `dispatch_billboard` (`td5_render_effects.c:1639`), camera-facing using the
  snapshotted secondary basis (`TD5RE_BILLBOARD_TREE_FIX`), **depth-sorted translucent
  buckets** — i.e. drawn AFTER the deferred shadow/light/SSR passes run.
- **Outside both shadow systems today**: not fed to the TLAS (neither cast nor receive
  RT shadows) and excluded from the G-buffer (degenerate billboard normals pack
  `pack[i]=0` → `gb.a<0.001` → the deferred passes treat those pixels as sky/lit).

### Materials

- `td5_material.c` (72 LOC): enum `TD5_MAT_` NONE/DEFAULT/CUTOUT/GLASS/GLOW/CARBODY.
  Classification is **page-transparency-class based** (`td5_material_id_for_page` maps
  `td5_asset_get_page_transparency`: opaque→DEFAULT, alpha-test→CUTOUT,
  translucent→GLASS, additive→GLOW), cached in `s_page_mat[1024]` (0xFF=unclassified).
  CARBODY is assigned by the render path (rotated actor spans), not by page.
- `TD5_MaterialParams k_params[]`: **reflectivity is field [2]** (the plan-1 misread is
  documented): DEFAULT=0.00, CUTOUT=0.00, GLASS=0.40, GLOW=0.00, CARBODY=0.30.
  No shininess/roughness is meaningfully consumed beyond that LUT.
- Env-map pages 990–993 exist as texture assets only (no runtime probe system).

### Lights

- `td5_light.c`: `TD5_DynLight s_lights[TD5_LIGHT_MAX=32]` registry;
  `td5_light_emit_vehicle_headlights` (L174–257) derives two forward spots per car from
  car-def hardpoints (+0x60/+0x68). Knobs `TD5RE_HEADLIGHT_RANGE`(11000)/`_INTENSITY`
  (0.95)/`_FWD`(400)/`_UP`(60)/`_CONE`(32°)/`_TILT`(0.40)/`_FWD_SIGN`/`_TRAFFIC`.
- Street lamps: `td5_light_lamps_*`, halo-page classified, `TD5_LAMP_MAX 4096`,
  nearest-N budget, **off by default** (`s_street_lights=0`, INI `[Lighting]
  StreetLights`).
- In HIGH, `rgen_light` already does per-light shadow rays; the light SET is unchanged
  from LOW.

### Reflections & their limits

- LOW: screen-space march `ps_ssr.hlsl`, `TD5RE_SSR_STEPS/_DIST/_THICK/_INTENSITY`
  (24/4000/500/0.8) — **this is the historical "only close, angle-dependent" system**.
- HIGH: `rgen_refl` traces to `sr_params.y` max dist; hit shading is complete
  (bindless textured + sun shadow). Remaining HIGH gaps: the TLAS only contains road
  lanes + cars (so reflections show almost no world), and max-dist/ray-count are baked.

### Options / INI infrastructure

- Schema-driven `k_lighting_cfg[]` (`main.c:428-448`) — each entry = INI key +
  `--Key=N` CLI + read for free. **Persist is NOT schema-driven**:
  `td5_ini_persist_options()` writes each `[Lighting]` key BY HAND — every new key must
  be added there too (the `Quality` round-trip bug @7617d24a is the cautionary tale).
- Existing `[Lighting]` keys: Enabled, Headlights, DarkMode, Auto, Mode, SunShadows,
  ShadowStrength, LightOcclusion, Reflections, WetRoads, StreetLights, Quality.
- GRAPHICS OPTIONS (`Screen_DisplayOptions`, `td5_fe_menu.c:1846`) is FULL (7 rows +
  OK). New rows need a sub-screen. Precedent for adding a screen: SELECT CUP got its
  own enum (47) — see `TD5_ScreenIndex` in `td5_types.h` + the screen table in
  `td5_frontend.c` + `FRONTEND_SCREEN_GUIDE.md`. The 6-touchpoint selector-row recipe
  is in plan 1's Phase-4 as-built note.
- i18n: `TR()` + `re/assets/frontend/lang/es_AR.txt` + `python
  re/tools/gen_i18n_catalog.py`; LOW/HIGH→BAJO/ALTO keys already exist.

### Known traps (all already paid for once — do not pay twice)

- **Framedump PNGs carry backbuffer alpha** — flatten to opaque or read RGB directly;
  composites must keep `SrcBlendAlpha=ZERO, DestBlendAlpha=ONE`.
- **`git checkout td5re.ini` (or diff it) before trusting a golden FAIL** — working-tree
  ini edits cause false fails.
- **Selftest is pinned to LOW** (RT-HIGH trips the 8x-FF TDR watchdog); RT visuals are
  framedump-verified separately; `TD5RE_RT=1` under the suite = deliberate stress test.
- **TDR discipline**: chunked BLAS builds, `RTMARK:*` crumbs, no dispatch wider than the
  swapchain. Never blame the GPU — a DEVICE_HUNG is our draw blowing the 2s watchdog.
- The bindless page-register hook lives in `flush_immediate()` (`td5_render.c`) — the
  mesh handlers bypass `td5_render_bind_texture_page`.
- Worktree builds: stale COPIED `libddraw_wrapper.a` causes unrelated Present crashes —
  rebuild the wrapper fresh in any new worktree.
- Kill only your own PID; screenshots via `TD5RE_FRAMEDUMP=<path.png>` (desktop capture
  is black).

## 3. Global invariants (every phase)

- SIM untouched; **golden traces green in every phase** (full suite; any golden diff is
  your bug — revert and rethink).
- **LOW byte-identical** to `rt-lighting` tip: never modify `ps_shadow.hlsl` /
  `ps_light.hlsl` / `ps_ssr.hlsl`; every new behavior is gated on `td5_rt_active()` (or
  its own HIGH-only knob) or is dormant data collection proven identical (as the
  G-buffer was).
- Lint ratchets: no new `extern` in .c, no new `td5_game.h` includers, no new warnings.
- Memory: total RT budget stays **< 700 MB** (was <500; Phase 2 buys headroom
  explicitly — see its gate). All feeds gated on `td5_rt_active()`; LOW allocates
  nothing (the degrade-private-bytes lesson).
- Device-lost: every new resource torn down in `d3d12_dxr_shutdown`, re-fed via the
  generation counter. Run the `TD5RE_FORCE_DEVICE_LOST=1` drill in any phase that adds
  GPU resources.
- Every new tunable gets an env knob (`TD5RE_*`) when introduced and an INI/menu
  surface in Phase 8. Defaults = highest quality.
- Dispatches only where the deferred passes run today (in-race, per-pane; never
  frontend/FMV). Split-screen: per-pane rects, full-frame masks — mirror the existing
  passes.
- Commit messages: `d3d12(RT2-P<n>): <summary>` + the project trailers. Do not push;
  branch `rt-lighting2`.

---

## Phase 1 — Sky probe: sun detection, sunny/overcast classing, sun disc

**Goal**: at sky-load time, analyze the sky texture; produce (a) a classification
{SUNNY, OVERCAST, NIGHT}, (b) for SUNNY a sun direction inferred from the image (fixed
per track, elevation encodes noon vs afternoon), (c) a visible sun disc; overcast tracks
get a soft wide-penumbra fallback sun so shadows persist. This phase only changes the
SUN SOURCE and adds the disc — consumers (shadow/light/SSR CBs) are already wired.

### 1.1 Probe (`td5_render.c`, extend the existing `s_sky_luma` probe site)
At `td5_render_load_sky`, over the CPU-side pixels already in hand:
- Compute mean luma `L_mean`, peak luma `L_peak` over a small box-filtered copy
  (e.g. downsample to ≤128×128 to kill single-pixel speculars), and the peak's
  centroid `(u,v)` = luma-weighted centroid of the connected region ≥ 90% of `L_peak`.
- Classify: `NIGHT` if `L_mean` below the existing dark threshold family (reuse the
  `s_sky_luma` scale); `SUNNY` if `L_peak/L_mean ≥ R` (start R=1.8, env
  `TD5RE_SUN_RATIO`) AND `L_peak ≥ P` (start 200/255, `TD5RE_SUN_PEAK`); else
  `OVERCAST`. A clamped-white sky band can fool the ratio test — require the peak
  region's area to be < 20% of the image (a huge "peak" = bright overcast, not a sun).
- Store: `s_sky_sun_class`, `s_sky_sun_uv`, exposed via new `td5_render_sky_sun()`
  (class + a world dir, see 1.2). Dump one log line per load:
  `TD5_LOG_RENDER("sky probe: class=%s peak=%u mean=%u uv=(%.3f,%.3f)")` — this is the
  per-track tuning surface.
- **Verification data first**: before wiring anything, run the probe over every track's
  sky (boot each level or, simpler, run the probe offline over the sky PNGs in
  `re/assets/` with a tiny C or Python harness in `re/tools/`) and eyeball the
  classification table vs the actual skies (Australia/Moscow/etc. sunny? which are
  overcast? night tracks?). Record the table in the as-built note. Tune R/P until the
  table matches reality. THIS TABLE IS THE PHASE'S CORE DELIVERABLE — the code is easy,
  the thresholds are the work.

### 1.2 UV → world direction
The sun's image position must become a world direction consistent with the dome
rendering. Derive the mapping from `s_sky_mesh` (`sky.prr`): find the dome vertex whose
UV is nearest `s_sky_sun_uv`, take its position direction from the dome center
(normalize; apply `TD5RE_SKY_PITCH` the same way the dome draw does). Compose with the
**current** `s_sky_rotation_angle` at frame time — the sun dir must rotate WITH the
dome so the disc and the shadow direction never drift apart. (The dome rotation is
slow ambience; if rotating shadows prove visually distracting, add
`TD5RE_SKY_ROT_FREEZE=1` to pin the dome when the probe sun is active — decide by eye,
note the decision.) Elevation from the mapped direction directly encodes noon
(steep) vs afternoon (shallow) — no separate time-of-day heuristic needed.

### 1.3 Sun-source override (HIGH only)
Where the shadow/SSR passes currently derive the sun from `s_tl_contrib` (the
strongest-zone-slot logic), insert: if `td5_rt_active()` and probe class is SUNNY,
use the probe sun dir (Y-flip convention: match the existing `sun[1]=-sun[1]` position
-space convention exactly — one sign error = shadows pointing INTO the sun; verify
against the disc in a framedump). If OVERCAST: keep the zone-derived dir (it encodes
the artists' intent) but widen the shadow cone (raise the `rgen_shadow` cone angle via
a new CB field, e.g. 4–6× the sunny cone) and cap shadow strength ≈ 0.5 — soft,
directionless-feeling shadows that still ground objects. NIGHT: unchanged (zone logic
already rejects below-horizon suns). LOW never reads the probe.
Env knobs: `TD5RE_SUN_PROBE=0` (fall back to zone sun in HIGH),
`TD5RE_SUN_OVERCAST_CONE`, `TD5RE_SUN_OVERCAST_STRENGTH`.

### 1.4 Sun disc
On SUNNY + HIGH, draw a sun disc + small glare at the probe direction: a camera-facing
additive billboard rendered with the sky (before world, after dome — position at
far-plane distance along the sun dir from the camera). Reuse the existing billboard
draw path or a 2-tri immediate draw with an additive blend state the backend already
has. Size `TD5RE_SUN_DISC_SIZE` (default ~3° angular), gated `TD5RE_SUN_DISC=1`
default on. Also pass the probe sun color (mean RGB of the peak region) into
`miss_refl` via SSRCB so RT reflections show a sun glint instead of the flat dark-blue
constant (small CB addition, RT-only).

**Files**: `td5_render.c` (probe + accessor), `td5_render_effects.c` (disc draw at the
dome site), `td5_render_mesh.c` (sun-source override in shadow/SSR pass setup),
`rt_common.hlsli`/`rt_pipeline.hlsl` (overcast cone CB field, miss sky color),
`td5_config.c/.h` (knobs), optional `re/tools/sky_probe.py` (offline threshold tuning).

**Gate**: build_all clean + lint; full suite green, goldens 54/0 (probe is HIGH-only +
load-time read-only); the per-track classification table recorded and sane; framedumps
on a sunny track (disc visible, shadows point away from the disc — pick a track where
the sun is clearly off-zenith to catch the sign), an overcast track (no disc, soft
shadows present), a night track (unchanged); `TD5RE_SUN_PROBE=0` restores prior HIGH
behavior; debug layer clean.

---

## Phase 2 — Full-scene RT geometry feed (scenery + cutout billboards)

**Goal**: the TLAS stops being "road lanes + cars" and becomes the whole visible world:
track/scenery span geometry (buildings, walls, bridges, terrain) and billboard
trees/signs (as cutout geometry, activating the dormant `anyhit_cutout`). This is the
foundation for Phases 3/4/6 — shadows from bridges/buildings, GI occlusion, and
reflections that show the world. **Riskiest phase — memory and TDR discipline apply.**

### 2.1 Scenery feed (`td5_rt.c` — extend `td5_rt_level_build`)
- Emit the full span-table geometry, not just lane quads: walk the same span table with
  the actual per-span polygon data (the road lanes ARE spans; scenery/wall/building
  spans are siblings — locate the span-type discrimination used by
  `td5_track_get_lane_quad_world` / `get_quad_vertices` and emit every renderable span
  type). 24.8→float `/256.0f`, +Y down, extent>12000 rejection stays.
- Attach the real `texture_page_id` per range (the page registry + bindless slots
  already exist) so `chit_refl` reflections of scenery are textured for free, and tag
  ranges whose page transparency class is alpha-test with a CUTOUT flag
  (`BackendRTRange.matid_flags`) for 2.2.
- One static "world" BLAS set, built once at level load through the existing chunked
  path (≤500k tris/frame — a big track may take several frames of warm-up; that's the
  accepted lazy-build pattern from plan 1).
- Retained prop meshes (if any world props render via the retained mesh registry rather
  than spans — verify by counting what the feed misses in a debug-view framedump):
  instance them like actor meshes but static.

### 2.2 Cutout billboards
- Feed each world billboard (tree/sign) as a **static crossed-quad pair** (two
  perpendicular vertical quads at the billboard's world position, sized from its
  billboard params) with its page id + CUTOUT flag. Static crossed quads — NOT
  camera-facing per frame — so shadows are stable under camera motion (this is the
  standard tree-shadow trick; a camera-facing shadow caster would swim).
- Activate `anyhit_cutout`: sample the bindless page texture's alpha at the hit UV,
  `IgnoreHit()` when alpha < 0.5. Cutout ranges get `D3D12_RAYTRACING_GEOMETRY_FLAG`
  **without** OPAQUE (everything else stays OPAQUE so shadow rays keep their
  early-accept fast path). Shadow rays must NOT skip anyhit on cutout geometry —
  keep `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` but drop `FORCE_OPAQUE` if it was
  used anywhere.
- Billboard source: enumerate at level load from wherever the retained opcode-4
  billboard commands source their world positions (the mesh command lists), not per
  frame.

### 2.3 Budget & instance cap
- TLAS instance cap 128 will not survive this phase — raise to 1024 (the double-buffer
  + PREFER_FAST_BUILD pattern holds; TLAS build stays ~0.1–0.3 ms at 1k instances).
- Pools: 128/48 MB VB/IB may need growth for big tracks — measure first, grow only to
  what the largest track needs (+ margin), record per-track feed sizes in the as-built
  note. Hard budget: total RT < 700 MB (`log/` the pool high-water via
  `TD5RE_RT_DIAG`).
- Perf: re-measure HIGH FPS + TLAS/pass times on the heaviest track (Moscow) and one
  billboard-heavy track. Target: shadows ≤2 ms, reflections ≤3 ms still hold with the
  full scene; if reflections blow the budget, clamp reflection TMax (Phase 6 makes it a
  knob anyway) — do not cut the shadow feed.

**Gate**: build_all + lint; full suite green (LOW allocates nothing — verify
degrade-private-bytes stays flat, this exact regression happened before), goldens 54/0;
`TD5RE_RT_DEBUGVIEW=1` framedump shows the full world silhouette (buildings/bridges/
trees) aligned over the raster; device-lost drill re-feeds; 10-min HIGH soak on Moscow
(no TDR, VRAM stable); memory table recorded.

---

## Phase 3 — Unified shadow treatment

**Goal**: user decision #3 — every element receives and casts shadows equally. After
Phase 2, casting is done (everything is in the TLAS). This phase fixes RECEIVING for
the two excluded classes: billboards/translucents (drawn after the deferred passes) and
any G-buffer-excluded geometry.

### 3.1 Billboards receive (shadow-mask modulation at draw time)
The sunvis mask (R32F, full-frame) already exists when translucent buckets draw. Make
it available to the translucent path: a new PS variant for billboard/translucent draws
that samples the sunvis mask at `SV_Position.xy` and modulates the diffuse RGB by
`lerp(1, shadow_strength_factor, 1-sunvis)` — same strength the opaque composite uses
(read from the same ShadowCB). Backend side: bind the sunvis SRV during translucent
draws when `s_rt_mode` (a new bit in the PSO key or reuse of the pass-SRV slot —
follow the G-buffer bit-22 precedent). HIGH-only: LOW keeps the existing PS
byte-identical. VFX that must NOT darken (additive glows, fire, headlight halos):
gate the variant on blend state — **only SRCALPHA_INVSRC translucents get it;
additive draws are light sources and stay unshadowed.**

### 3.2 G-buffer coverage for billboards
Give billboard pixels a G-buffer entry so Phase 4's GI applies to them: in the
billboard vertex path, pack an up-normal (0,255,0-style) + CUTOUT matid instead of
`pack[i]=0` **only when HIGH** (the `_g` PS only writes RT1 where bound; LOW never
binds it — but keep the pack change itself HIGH-gated to honor byte-identical LOW,
mirroring how COLOR1 packing is already mode-gated).

### 3.3 Sweep for remaining exempt classes
With 3.1+3.2 in, framedump-audit one busy scene per element class: car on road ✓,
car under bridge (bridge shadow falls on car AND road AND a tree billboard), scenery
wall shadow onto road, tree shadow onto car roof, banner/sign shadow. Fix stragglers
found (e.g. banners drawn through a different path). Record the audit matrix in the
as-built note.

**Gate**: build_all + lint; suite + goldens green; the audit framedump matrix (every
combination above shows the shadow); no double-darkening (a billboard in shadow must
not multiply both the mask AND a baked dark vertex color into black — if TD6 baked
grey already darkens an under-bridge tree, the mask modulation on top is acceptable
only if it reads right; judge by eye, note the call); debug layer clean; FPS re-check.

---

## Phase 4 — RT sky-visibility GI + darkening replacement in HIGH

**Goal**: user decisions #2 and #7. A per-pixel ray-traced sky-visibility (ambient
occlusion toward the sky) term makes outdoor areas bright and covered areas dark —
physically, from the Phase-2 scene — and REPLACES the zone dark-mode dimming and
synthetic darkening on cars in HIGH.

### 4.1 `rgen_ao`
New raygen: for each pixel (depth+G-buffer reconstruct, same as `rgen_shadow`), cast K
cosine-weighted hemisphere rays around the G-buffer normal (K = `TD5RE_RT_GI_RAYS`,
default 4; stratified like the shadow sampler), TMax = `TD5RE_RT_GI_DIST` (default
~6000 world units — sky-visibility wants medium range: a bridge deck occludes, a
mountain 50k away shouldn't). Miss = sky contribution 1.0; hit = 0. Output: R8 `gi`
mask, average of K. New raygen SBT record + UAV u4 + heap slot — follow the exact
pattern `rgen_light`/`rgen_shadow` used (64-strided raygen record, fixed-table growth).
Optional cheap denoise: reuse the source-multisampling stance from P2b (K samples at
source, no blur pass) — raise default K rather than adding a filter.

### 4.2 Composite
Multiplicative composite over the pane (same slot in the pass chain as the shadow
composite, immediately after it): `rgb *= lerp(gi_floor, 1.0, gi)` with `gi_floor` =
`TD5RE_RT_GI_FLOOR` (default 0.45 — full occlusion never goes black; tune vs the LOW
dark-mode look which used `s_dark_scale` 0.50). HIGH-only PSO via
`dxr_make_composite_pso` (dest-alpha-preserving, as always).

### 4.3 Retire the analytic darkeners in HIGH
When `td5_rt_active()`:
- Skip zone dark-mode dimming for ACTORS (the car path: `s_light_dark_mode` /
  `s_dark_scale` application in the car vertex-lighting block) — the GI mask now
  darkens the car's pixels under the bridge, from actual geometry, per-pixel. This is
  the Australia-bridge fix: cars darken exactly where covered, gradually at the edges.
- Keep the synthetic 3-dir diffuse itself (it's the car's shading/normal response, not
  the offender) and keep paint tint + damage scuff.
- Track/scenery TD6 baked vertex grey STAYS (it is authored art, and under-bridge road
  darkening from it + GI on top must be judged by eye — if double-dark, attenuate the
  GI term where baked grey is already < threshold, i.e. `gi' = max(gi, 1-baked_dark)`;
  decide from framedumps, note the decision).
- Auto-headlight darkness detection (`td5_render_env_is_dark_for_slot`) is untouched —
  it's an input trigger, not a render effect.

**Gate**: build_all + lint; suite + goldens green; A/B framedumps at the Australia
start bridge (HIGH: car+road darken under deck with soft edges, brighten on exit; LOW:
unchanged legacy look); a tunnel (GI floor holds, headlights still auto-on); open road
(GI ≈ 1.0, no dimming); perf ≤2 ms for the AO pass at default K (measure, record);
10-min soak; debug layer clean.

---

## Phase 5 — Material shininess detection

**Goal**: user decision #5 — infer per-page shininess/reflectivity by inspecting the
textures, replacing the coarse per-class constants for reflectivity. Detection targets:
car chassis (keep/refine CARBODY), car & building windows, scenery glass, water.

### 5.1 Classifier (load-time, `td5_material.c` + `td5_asset.c` hook)
At page-upload time (the same site that feeds `td5_asset_get_page_transparency`),
compute per-page stats over the decoded pixels: mean RGB, saturation, luma variance,
alpha class (already known), and cheap structure hints (fraction of near-black pixels,
fraction of blue-dominant pixels). Heuristics (ordered, first match wins; every
threshold an env knob for tuning):
- **WATER**: blue/cyan-dominant mean (B > R and B ≥ G) + low-to-mid luma variance +
  opaque class + used by track spans that are near-horizontal (the up-facing check
  exists in the light-basis path). Reflectivity 0.55, high shininess.
- **GLASS/WINDOW**: translucent class (today's rule, keep) OR opaque pages whose name
  (zip entry name, if plumbed to the page registry — check; if not, skip name hints)
  contains `glass`/`win` OR very low saturation + high variance grid-like windows on
  building pages — start conservative: translucent-class + name-hint only; the
  building-window-from-statistics idea graduates only if the dump (5.2) shows it's
  reliable. Reflectivity 0.40 (unchanged), shininess high.
- **CARBODY**: stays render-path-assigned (rotated actor spans). Refine within it:
  windshield/window ranges of the car mesh get GLASS if their page classifies as such.
- **Default**: today's mapping.
Output: `s_page_shine[1024]` (u8 shininess) + `s_page_refl[1024]` (u8, replaces the
per-class constant when set), consumed by the SSR/refl classifier where matid→LUT is
read today (both LOW's `ps_ssr` CB inputs and RT's `rt_reflectivity` — **CAREFUL:
feeding new per-page reflectivity to LOW changes LOW** — so the per-page tables are
HIGH-only; LOW keeps the class LUT byte-identical).

### 5.2 Tuning surface
`TD5RE_MAT_DUMP=1` writes `log/material_pages.csv`: page id, class, stats, verdict,
and (if available) source entry name. Run per track, eyeball, tune thresholds. Record
per-track verdict counts + notable pages in the as-built note. This CSV is the phase's
deliverable quality proof — the Australia water, a city glass tower, and a car
windshield must all classify correctly, and false positives (blue billboards ≠ water)
must be enumerated.

### 5.3 Feed into RT
`GeoRecord` already carries `texture_index`; add the page shininess/reflectivity to
the GeoRecord (or index the page tables from the shader via a small StructuredBuffer
upload — pick whichever avoids re-feeding geometry). `rgen_refl` uses per-page
reflectivity when present; `chit_refl` sharpens/softens the reflected sample by
shininess (perfect mirror for glass/water; for lower shininess, keep single-ray but
attenuate intensity — real glossy multi-ray is out of scope, note it).

**Gate**: build_all + lint; suite + goldens green (LOW LUT untouched — assert
byte-identical by A/B framedump of a LOW race); the CSV audit; HIGH framedumps: water
reflecting the world, building glass reflecting, car windshield distinct from body,
matte road NOT reflecting when dry; debug layer clean.

---

## Phase 6 — Reflection & shadow range/angle completeness + precision knobs

**Goal**: user decision #4 — kill the legacy near-distance / camera-angle limits. After
Phase 2 the TLAS is scene-complete, so this phase is verification + de-hardcoding:
every remaining range cap becomes a knob, defaults set to "no visible limit".

- Shadow rays: verify TMax reaches the sun "infinitely" (TMax = large constant, e.g.
  1e7 — confirm it already is; if a scene-diagonal cap exists, remove it). Distant
  casters (a bridge 20k units ahead) must shadow correctly — framedump proof.
- Reflection rays: `sr_params.y` TMax → `TD5RE_RT_REFL_DIST` (default 50000 — beyond
  the far plane, i.e. unlimited in practice), reflection ray count 1 → keep (note:
  precision option in Phase 8 maps to {half-res, full-res} dispatch + TMax tiers, not
  multi-ray).
- Half-res option plumbing: allow the refl (and optionally AO) dispatch at half
  resolution with a bilateral-ish upsample in the composite (depth-aware lerp in the
  existing composite PS). This is the "reflection precision" lever Phase 8 exposes;
  default FULL.
- View-independence check: the historical bug was screen-space (off-screen geometry
  can't reflect/shadow). RT is inherently view-independent — prove it with the framedump
  pair: camera angled so the caster/reflected object is OFF-screen, shadow/reflection
  still present. This pair goes in the as-built note as the closure proof of the
  original complaint.
- LOW untouched: the SSR march keeps its 4000-unit horizon; that IS low quality now.

**Gate**: build_all + lint; suite + goldens green; the off-screen caster/reflector
framedump pair; distant-shadow framedump; half-res A/B (no edge crawl worse than
acceptable — judge, note); FPS at full vs half res recorded; debug layer clean.

---

## Phase 7 — HIGH light pipeline: headlights, street lights, sun as realistic lights

**Goal**: user decision #8. LOW keeps today's system byte-identical. HIGH upgrades the
RENDERING of the existing light registry — the registry itself (`td5_light.c` emitters,
hardpoints, auto-on logic) remains the single source.

- **Headlights (HIGH)**: `rgen_light` already does attenuation+cone+Lambert+shadow ray.
  Upgrade: per-light K shadow samples (soft penumbra; reuse the stratified sampler,
  K = `TD5RE_RT_LIGHT_RAYS` default 2), a proper projected-cone falloff (smooth inner/
  outer angle instead of the current cone edge — CB field, RT shader only), and ground
  "pool" quality: verify the two spots per car actually read as beams on the road at
  night with occlusion (the owed night/tunnel visual from plan 1 — close it here).
- **Street lights (HIGH)**: flip `s_street_lights` default ON when `td5_rt_active()`
  (LOW default stays OFF), nearest-N budget → the RT light cap (`RT_LIGHT_MAX=32`
  shared; keep the existing nearest-N selection). Night city framedump: lamp pools
  with real occlusion (a car under a lamp casts a lamp shadow).
- **Sun**: already the shadow pass; ensure sun INTENSITY/color from the Phase-1 probe
  feeds the composite (sunny = crisper/warmer, overcast = flat) — small CB fields.
- **VFX halos**: the additive halo sprites on lamps/headlights stay (they're the
  visible source); only illumination is upgraded.

**Gate**: build_all + lint; suite + goldens green; night race framedumps (headlight
beams + occlusion; street-lit city block; a tunnel with working headlights + GI floor);
FPS at night with max lights recorded (the light pass is the expensive one — if >3 ms,
lower default K to 1 and note); device-lost drill; debug layer clean.

---

## Phase 8 — LIGHTING OPTIONS screen, INI schema, defaults, release

**Goal**: user decision #6 — every feature independently tunable, all defaulting to
highest. GRAPHICS OPTIONS is full → add a **LIGHTING OPTIONS sub-screen**.

### 8.1 New screen
New screen enum (follow the SELECT CUP precedent: `TD5_ScreenIndex` + screen table +
`FRONTEND_SCREEN_GUIDE.md` update) reached from a new GRAPHICS OPTIONS row (replace the
current LIGHTING QUALITY row 6 with `LIGHTING OPTIONS →`; QUALITY moves inside). Rows
(each the 6-touchpoint recipe; L/R live-apply where the knob is runtime-safe, which all
of these are):
1. LIGHTING QUALITY: LOW / HIGH (moved here; greyed w/ lock cue when no DXR)
2. SHADOW QUALITY: LOW(1 ray) / MEDIUM(4) / HIGH(8) → `[Lighting] ShadowRays`
3. SHADOW RESOLUTION: HALF / FULL → `[Lighting] ShadowRes` (dispatch res, P6 plumbing)
4. REFLECTIONS: OFF / HALF / FULL → `[Lighting] ReflectionQuality` (res tier + the
   existing Reflections enable)
5. REFLECTION RANGE: NEAR / FAR / UNLIMITED → `[Lighting] ReflectionRange` (TMax tiers)
6. GLOBAL ILLUMINATION: OFF / LOW(2) / HIGH(4+) → `[Lighting] GIQuality` (rays; OFF
   restores zone dark-mode in HIGH so cars still darken under bridges)
7. SUN & SKY: AUTO / CLASSIC → `[Lighting] SunProbe` (CLASSIC = zone sun, no disc)
8. LIGHTS: BASIC / REALISTIC → `[Lighting] LightQuality` (P7 features + street lights)
All rows inert/greyed at LOW except row 1 (they're HIGH features). Defaults: HIGH, 8
rays…? — **defaults = the highest tier of every row** per the user decision; if the
measured frame cost at all-max exceeds ~6 ms RT total on the 5070 Ti, keep all-max
anyway (user asked for highest default) but record the measured cost prominently.
es_AR strings via the catalog + `gen_i18n_catalog.py`.

### 8.2 INI & env unification
Every `TD5RE_RT_*` tuning env from Phases 1–7 that a row controls now reads:
CLI/INI value, env var still overrides for A/B (the `TD5RE_RT` precedent). Add every
new key to `k_lighting_cfg[]` AND to `td5_ini_persist_options()` (the hand-written
persist — the Quality round-trip bug must not repeat; grep-verify every new key
appears in BOTH sites). Round-trip test each: set in menu → OK → relaunch → verify.

### 8.3 Release & closure
- Release build: RT compiles in (already does); verify the new screen + knobs in
  `td5re_release.exe` (manual race entry; `--AutoRace` is dev-only).
- Close plan-1 owed items alongside: 30-min HIGH soak (no TDR, VRAM stable), literal
  mid-race LOW↔HIGH toggle via the control socket, RT-in-release visual.
- Update `EXPECTED_BEHAVIOR.md` (new options + what LOW vs HIGH means now) and
  `FRONTEND_SCREEN_GUIDE.md` (new screen). Regenerate the module table if any module
  was added. Update `.happy/project-info.json`.

**Gate**: build_all + lint; full suite green + goldens 54/0; every row live-applies
and round-trips; DXR-absent machine path (TD5RE_RT_DISABLE=1): screen reachable, all
HIGH rows greyed, LOW plays normally; release-build manual verification; the soak;
final FPS/memory table at all-max defaults recorded in the as-built note.

---

## Handoff prompt (paste to launch each execution session)

```
Read C:\Users\maria\Desktop\Proyectos\TD5RE\docs\plans\RT_LIGHTING2_PLAN.md in full,
then docs/plans/RT_LIGHTING_PLAN.md's as-built notes, then CLAUDE.md, before touching
anything. Execute the NEXT unstarted phase in RT_LIGHTING2_PLAN.md's Status table
(one phase only), on branch rt-lighting2 (create from rt-lighting if absent). Rules:

- One phase per session, one commit per phase: `d3d12(RT2-P<n>): <summary>` with the
  project's co-author trailers. Do not push.
- Do not start if the previous phase's Status row isn't ✅ with its as-built note
  appended. Finish by flipping your phase's Status row and appending your as-built
  note (deviations, measurements, tuning tables, gotchas).
- Gates are non-negotiable: build_all.bat clean with no new warnings,
  `pwsh scripts/selftest.ps1 -Suite full` green, golden traces green (any golden diff
  = your bug — revert and rethink; RT is render-only), plus the phase's framedump
  checks (TD5RE_FRAMEDUMP=<path.png>; desktop capture is black; flatten PNG alpha
  before eyeballing). `git checkout td5re.ini` before trusting a golden FAIL.
- LOW ([Lighting] Quality=0) must remain byte-identical to the rt-lighting tip in
  every phase; never modify ps_shadow.hlsl / ps_light.hlsl / ps_ssr.hlsl; all feeds
  and new passes gate on td5_rt_active().
- The frozen DXR decisions from RT_LIGHTING_PLAN.md §4 stand (SBT layout, global RS
  growth pattern, recursion depth 2, no local root signatures). Extend, don't
  redesign.
- Memory: LOW allocates nothing (verify degrade-private-bytes stays flat); total RT
  < 700 MB; chunked BLAS builds; RTMARK crumbs; run the TD5RE_FORCE_DEVICE_LOST=1
  drill in any phase adding GPU resources.
- Use the td5re MCP live-control server (dev build, --Control=1) for in-game
  scenarios; kill only your own PID, never by image name.
- If a capability you need is denied by managed policy, stop and report exactly what
  is blocked. Never fabricate build/test output.
```
