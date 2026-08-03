# RT sun-shadows: "objects don't cast onto the road" — diagnosis & open bug

_Status: **OPEN** (previously unsolved — see the CSM-shadow history). Root cause
narrowed but not fully cracked. Written 2026-08-03 after a long diagnostic session
on branch `rt-lighting2`. Reusable debug tooling is committed; use it to continue._

## Symptom (user report)

In HIGH (RT) lighting on Australia/Sydney (DefaultTrack=2): **no cast shadows** —
buildings, the car, and road structures don't drop shadows onto the road. Only
faint "glimpses at short distance." (Also: the legacy Australia zone-shading came
back when GI was off — that was a *separate* issue, fixed by re-enabling GI once
the GI SBT-hang was fixed; see commit `81d70f45`.)

## How to reproduce + inspect (the debug tooling — all committed)

Drive the AI car into the city with the control socket, capture with the opaque
"raw mask" composite so the shadow buffer is shown directly (no scene multiply):

```
# launch: td5re.exe --Control=1 --SkipIntro=1 --LightingQuality=1
# env for a clean readout:
TD5RE_D3D12_CAPTURE=1     # arm per-present backbuffer capture (framedump needs this in-race)
TD5RE_RT_MASK=1          # shadow composite = OPAQUE (writes the mask directly, no MULT)
TD5RE_RT_SHADOW_DEBUG=<level>
```

`TD5RE_RT_SHADOW_DEBUG` levels (rgen_shadow in `rt_pipeline.hlsl`; CPU flag in
`td5_platform_win32.c` apply_shadow, passed as a negative `sh_params.x`):

| level | output (written to g_sunvis, read via TD5RE_RT_MASK=1) |
|---|---|
| 1 | raw full-contrast visibility (black = shadow ray hit, white = lit/reached-sky) |
| 2/3/4 | reconstructed **world .x / .y / .z** (value×262144 / ×65536 / ×262144) |
| 5 | **G-buffer normal validity** = length(raw normal); 1=unit (good), 0=zero (bad) |
| 6/7/8 | encoded G-buffer normal .x / .y / .z (0.5 = zero component) |

The `[shadowdiag]` one-shot log (TD5RE_RT_DIAG=1) prints sky class / sun dir /
dominance / strength from `td5_render_apply_shadow_pass` (td5_render_mesh.c).

Drive with: `start_race {track:2, player_is_ai:1, auto_throttle:1}` then wait ~20s
(the AI car leaves the start-line bridge and reaches the city where buildings line
the road). AutoRace leaves the player car stationary at the start — you MUST use
`player_is_ai` to move it.

## What was RULED OUT (with evidence)

1. **Depth reconstruction is CORRECT.** Level-3 (world.y) readout: the road renders
   grey ≈ 0.09 → 0.09×65536 ≈ **5900**, matching the HUD `POS.y = 5869.2`. And the
   code confirms it: the port writes **linear** view-space Z (`(vz-64)×(1/195000)`,
   td5_render.c:947), and `rt_world_from_depth` inverts it exactly (`D×195000+64 =
   vz`). **Do NOT "fix" rt_world_from_depth** — it's correct and shared by the
   light + reflection passes.
2. **G-buffer coverage** — the road IS covered (level-1 no-gbuffer marker didn't
   grey it).
3. **Sun direction / classification** — `[shadowdiag]`: sky_cls=SUNNY, sun ≈ 33°
   elevation, dominance 0.9, strength 0.40. Correct.
4. **Occluders are in the TLAS** — scenery feed logs `SCENERY_FEED ... solid=966
   billboards=778`, and buildings self-*react* to the shadow pass.

## What was FOUND (confirmed)

**G-buffer surface normals are bad for mesh geometry (cars + scenery), good for
the road.** Level-5 (normal validity): road = white (unit), car = mid-grey (~0.5
interpolated), buildings = **salt-and-pepper** (many mesh vertices have zero /
missing normals). Source: `td5_render_compute_vertex_lighting`
(td5_render_mesh.c:829-859) only packs a normal when it's non-zero; zero-normal
vertices get `pack=0` (= "no G-buffer / emissive"). Scenery meshes either lack
normals (and `td5_track_derive_missing_normals` doesn't cover them / produces zero
for degenerate faces) or carry authored-but-bad normals.

Bad/zero normals → the RT shadow-ray origin bias (`origin = world + N·bias`) points
wrong → the ray self-hits → **acne** → objects render as uniform dark blobs in the
shadow pass instead of self-shadowing/casting.

## The TWO (or three) distinct problems

1. **Building acne** — *bias-magnitude sensitive*: with `TD5RE_RT_BIAS=12` the
   buildings mostly go white (self-shadow correctly). So a bigger/robust bias
   helps them (but risks peter-panning; not a clean default bump).
2. **Car acne** — *bias-IMMUNE*: normal bias, 12× bias, and a view-direction origin
   offset ALL leave the car uniformly black. So the car is a special case — its
   shadow ray hits regardless of where the origin is placed. Suspect the actor /
   depth path (chase-cam-close geometry; the car has its own TD6_CAR_ZFIX depth
   handling, though zfix is likely 0 for TD5 cars). **Unexplained — needs its own
   probe** (dump the car pixel's reconstructed world.xyz via levels 2/3/4 and
   compare to the actor's known world position; check whether the car is even fed
   into the TLAS at the reconstructed position).
3. **Road receives NO cast shadow** — *the actual user complaint*, and it is
   **UNAFFECTED by any acne fix**. Even with buildings self-shadowing (bias 12×),
   the road stays fully lit. So this is a **separate occlusion problem**: a road
   pixel (correctly reconstructed) casts a ray toward the sun and misses occluders
   that are visually adjacent. Since reconstruction is correct and occluders are in
   the TLAS, the leading suspect is that **scenery geometry is fed into the TLAS at
   a position/scale that doesn't match where it renders** (the P2 feed's documented
   "world units / truncated x64 pointer" gotchas). CONFIRM by: read a building
   pixel's reconstructed world.xyz (levels 2/3/4) and compare to that building's
   vertex positions as fed by `rt_build_scenery_mesh` / `rt_feed_world_scenery`
   (td5_rt.c). A mismatch (offset or 256× scale) is the bug.

## Fix attempts that FAILED (don't repeat)

- **Renormalize the packed world normal** (td5_render_mesh.c) — no effect; the
  shader already `normalize()`s the interpolated normal, so magnitude is moot.
- **Bump shadow-ray bias** — helps buildings only; not the car, not road-cast.
- **View-direction origin offset** (`origin = world + vdir·bias`) — car still
  black; buildings worse than a plain bias bump.

## Recommended next steps (in order)

1. **Road-cast (problem 3) is the real target.** Compare a building pixel's
   reconstructed world.xyz (debug levels 2/3/4) against the same building's TLAS
   vertex positions (instrument `rt_build_scenery_mesh` to log a sample vertex).
   If they differ → fix the scenery feed's coordinate convention. This is almost
   certainly THE bug for the user's complaint.
2. **Scenery normals (problem 1)** — make `td5_track_derive_missing_normals` also
   cover meshes that have *bad* authored normals (renormalize / re-derive when
   `|N|` is far from 1), or skip zero-normal vertices' acne with a robust
   ray-space bias. Improves object self-shadowing.
3. **Car (problem 2)** — dedicated probe of the car's reconstructed position vs its
   TLAS/actor position; it's a special case.

## Related commits (this session, branch `rt-lighting2`)

- `81d70f45` GI SBT collision fix (GI hung the GPU — separate, now fixed)
- `9e728ba2` TDR-safe defaults + wall-clock device-lost recovery
- `ef006f5d` device-lost graceful fail (no more freeze/reboot)
- `463426bb`/`56bab227` per-frame log-spam gating
- `e5c44d53`/`1ecc216c`/`4bd9d2e0` the shadow debug tooling used above
