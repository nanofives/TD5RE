# Weather / Ambient Particle System

**Date:** 2026-03-20
**Binary:** TD5_d3d.exe (port 8195)
**Status:** Fully analyzed. Snow confirmed as CUT FEATURE with partial init code but zero render code.

## Overview

The weather system renders camera-relative ambient particles (rain streaks) during races on weather-enabled tracks. Weather type and per-segment density are driven by `LEVELINF.DAT` fields packed into each level ZIP. Three weather modes exist in the data format but only rain (type 0) has a complete implementation.

**Weather types:**
| Value | Type | Status |
|-------|------|--------|
| 0 | Rain | Fully implemented: init, density, render, audio |
| 1 | Snow | **CUT**: init allocates buffers + seeds positions, but render is gated off. No sprite quads built. |
| 2 | Clear | No particles, no audio. Default for most tracks. |

---

## Function Reference

| Address | Name | Purpose |
|---------|------|---------|
| 0x446240 | `InitializeWeatherOverlayParticles` | Allocates particle buffers, seeds positions, builds sprite templates (rain only) |
| 0x4464B0 | `UpdateAmbientParticleDensityForSegment` | Per-sim-tick: adjusts active particle count based on track segment |
| 0x446560 | `RenderAmbientParticleStreaks` | Per-frame: advances particles in camera space, projects to screen, queues translucent quads |
| 0x42A6B0 | `SpawnAmbientParticleStreak` | Allocates one particle in the general race particle pool (for rain splash effects) |
| 0x429510 | `InitializeRaceParticleSystem` | Inits RAINSPL + SMOKE sprites, clears per-view particle slots |
| 0x441C60 | `LoadRaceAmbientSoundBuffers` | Loads Rain.wav (and other ambient sounds) from SOUND.ZIP |
| 0x440E00 | `UpdateVehicleAudioMix` | Gates rain ambient audio on weather_type == 0 |

### Callers

- **`InitializeWeatherOverlayParticles`** called from `InitializeRaceSession` (0x42b2ec) -- once at race start
- **`UpdateAmbientParticleDensityForSegment`** called twice per sim tick from `RunRaceFrame` (0x42ba3c, 0x42ba5e) -- once for each player view (P1=view 0, P2=view 1)
- **`RenderAmbientParticleStreaks`** called twice per render frame from `RunRaceFrame` (0x42be9a, 0x42c0e7) -- once per viewport pass, between `RenderRaceActorsForView` and `DrawRaceParticleEffects`
- **`SpawnAmbientParticleStreak`** called only from `RenderAmbientParticleStreaks` (0x44662b) -- probabilistic spawn during rain rendering

---

## Particle Pool Structure

### Weather Particle Buffer

Two identical buffers are allocated, one per view (split-screen support):

| Global | Description |
|--------|-------------|
| `DAT_004c3dac` | Particle buffer pointer, view 0 |
| `DAT_004c3db0` | Particle buffer pointer, view 1 |

Each buffer: **0x6400 bytes** (25,600 bytes) = **128 particles x 200 bytes/particle**

**Particle slot layout** (200 bytes = 0xC8, stride 0x32 dwords):

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| +0x00 | float | pos_x | Camera-relative X position [-4000, +4000] |
| +0x04 | float | pos_y | Camera-relative Y position [-3000, +3000] |
| +0x08 | float | pos_z | Camera-relative Z (depth) [-1947, +1947] |
| +0x0C | float | visible | Visibility flag: 0.0 = out-of-bounds (recycle), nonzero = active |
| +0x10 | ... | quad_template | Sprite quad vertex data (built by `BuildSpriteQuadTemplate`, ~184 bytes) |

**Rain init ranges:** X: [-4000, 4000], Y: [-3000, 3000], Z: [-1947, 1947] (via `rand() % N`)
**Snow init ranges:** X: [-8000, 8000], Y: [-8000, 4000], Z: [200, 8143] (wider spread, higher minimum altitude)

### Maximum Active Particles

The active particle count per view is stored at:

| Global | Description |
|--------|-------------|
| `DAT_004c3dd8` | Target density, view 0 (set by density pairs) |
| `DAT_004c3ddc` | Target density, view 1 |
| `DAT_004c3de0` | Current active count, view 0 (ramped toward target) |
| `DAT_004c3de4` | Current active count, view 1 |

Maximum: **128 particles** (0x80). Density values from LEVELINF.DAT are clamped to 128 at runtime.

---

## Per-Segment Density System

### LEVELINF.DAT Fields

```c
struct LevelInfoDat {
    /* +0x28 */ uint32_t weather_type;         // 0=rain, 1=snow, 2=clear
    /* +0x2c */ uint32_t density_pair_count;   // Number of (segment, density) pairs
    /* +0x34 */ struct {
                    int16_t segment_id;        // Track segment triggering change
                    int16_t density;           // New target particle count
                } density_pairs[6];           // Max 6 pairs
};
```

### UpdateAmbientParticleDensityForSegment (0x4464B0)

**Signature:** `void __cdecl UpdateAmbientParticleDensityForSegment(int actor_ptr, int view_index)`

**Logic:**
1. Reads the actor's current track segment from `actor_ptr + 0x80` (int16 span index)
2. Walks the density pair array at `gTrackEnvironmentConfig + 0x36` (count at +0x2c)
3. Each pair is 4 bytes: `[int16 segment_id, int16 density]`
4. When the actor's segment matches a pair's segment_id, the target density is updated
5. Target clamped to max 128
6. Active count is incremented by 1 per call (if below target) or decremented by 1 (if above)
7. When incrementing, the newly-activated particle's visibility flag (+0x0C) is zeroed to trigger re-seeding

This creates **zone-based weather intensity**: as the player drives through different track segments, particle density ramps up or down. Rain tracks typically have 6 pairs creating gradual transitions.

### Example: level001 (Rain)

Typical rain track has 6 density pairs ramping from light to heavy and back.

### Example: level003 (Snow -- CUT)

```
density_pairs[0] = { segment=2300, density=0 }
density_pairs[1] = { segment=2400, density=128 }
```

Snow starts clearing at segment 2300, reaches full density at segment 2400. These ARE processed at runtime -- the active particle count IS tracked -- but particles are never rendered.

---

## Rain Streak Rendering

### RenderAmbientParticleStreaks (0x446560)

**Signature:** `void __cdecl RenderAmbientParticleStreaks(int actor_ptr, float sim_budget, int view_index)`

**Called from:** `RunRaceFrame` once per viewport pass (after track/actors, before HUD)

#### Camera Position Interpolation

Uses `g_subTickFraction` (0x4AAF60) for sub-tick interpolation:
```c
cam_x = (1/256.0) * (actor[0x1CC] * g_subTickFraction + actor[0x1FC]);
cam_y = (1/256.0) * (actor[0x1D0] * g_subTickFraction + actor[0x200]);
cam_z = (1/256.0) * (actor[0x1D4] * g_subTickFraction + actor[0x204]);
```

#### Streak Direction Computation

Rain streaks derive their screen-space direction from camera motion:
1. **Wind vector** (constant): `(0.0, -250.0, 0.0)` at 0x474E58-0x474E60. Rain falls straight down with no horizontal drift.
2. **Camera delta**: `prev_cam_pos - current_cam_pos` (stored per-view at `DAT_004c3db8 + view*0xC`)
3. **Combined motion**: `wind * sim_budget - camera_delta`
4. **View-space transform**: Rotated by `LoadRenderRotationMatrix` / `TransformVector3ByRenderRotation` into screen-aligned coordinates

#### Particle Update Loop

For each of the `active_count` particles:

1. **Advect**: Position += (view_space_motion * 4096.0) / `DAT_00467368` (world scale factor)
2. **Bounds check**: If |X| > 4000 or |Y| > 3000 or |Z| > 1947.5, particle is out of bounds
3. **Recycle**: Out-of-bounds particles get re-seeded with new random position
4. **Project**: Camera-relative 3D position is projected to 2D screen coordinates via perspective division:
   ```
   depth = Z + 2147.0
   screen_x = X * (180.0 / depth)
   screen_y = (Y - 1000.0) * (180.0 / depth)
   ```

#### Streak Quad Construction

Rain is rendered as **thin vertical line quads** (NOT billboards, NOT point sprites):

1. Two projected points are computed: the particle's **previous** and **current** screen positions
2. The point with the higher Y (lower on screen) becomes the "top" vertex
3. A quad is built with width = 1 pixel (`x +/- 0.5`) connecting the two projected points
4. `BuildSpriteQuadTemplate` constructs the vertex data using the RAINDROP sprite UVs
5. `QueueTranslucentPrimitiveBatch` submits the quad to the depth-sorted translucent render queue

**Result:** Each raindrop appears as a 1-pixel-wide angled streak whose length and angle depend on camera velocity. Faster camera motion = longer, more angled streaks. Stationary camera = short vertical lines.

#### Rain Splash Spawning

Each frame, a random check determines whether to spawn a splash particle:
```c
if ((rand() & 0x7F) <= active_count && active_count > 0) {
    SpawnAmbientParticleStreak(actor[0x318], view_index);
}
```

This creates `RAINSPL` splash effects in the general race particle pool (separate from the weather pool) using the callback-driven particle system at `DAT_004a318f`.

#### Pause Gate

When `g_audioOptionsOverlayActive != 0` (in-race options overlay), particle positions are frozen (not updated) and the previous frame's `sim_budget` is reused. This prevents particles from jumping when unpausing.

### Constants Table

| Address | Value | Usage |
|---------|-------|-------|
| 0x4749D0 | 1/256.0 (0.00390625) | Fixed-point to float conversion for camera coordinates |
| 0x474E58 | 0.0 | Wind X component (no horizontal drift) |
| 0x474E5C | -250.0 | Wind Y component (downward rain velocity) |
| 0x474E60 | 0.0 | Wind Z component |
| 0x474E64 | 180.0 | Perspective projection scale factor |
| 0x474E68 | 4000.0 | X bounds limit for particle recycling |
| 0x474E6C | 3000.0 | Y bounds limit |
| 0x474E70 | 1947.5 | Z bounds limit (near clipping of rain volume) |

---

## The Snow Gate (CUT FEATURE)

### Location: 0x4465F4

**Assembly:**
```asm
00446578:  A1 E8 3D 4C 00       MOV EAX, [DAT_004c3de8]    ; load weather_type
0044657D:  33 DB                 XOR EBX, EBX               ; EBX = 0
0044657F:  3B C3                 CMP EAX, EBX               ; weather_type == 0?
    ... (FPU setup for camera position interpolation) ...
004465F4:  0F 85 67 04 00 00    JNZ 0x446A61                ; if weather != 0, SKIP ALL RENDERING
```

The `JNZ` at **0x4465F4** jumps over the **entire** rain rendering loop (spawn check, particle advection, projection, quad building, translucent queue) to the function epilogue at **0x446A61**. This is a 0x467-byte jump (1127 bytes of skipped code).

**Decompiled equivalent:**
```c
if (DAT_004c3de8 == 0) {
    // ALL rain rendering code: ~110 lines
    // spawn, advect, project, build quads, queue
}
return;  // No else branch. No snow renderer. Nothing.
```

### Why Snow Cannot Render

Three independent problems prevent snow from ever appearing:

1. **Render gate (0x4465F4):** The `JNZ` skips all particle rendering when `weather_type != 0`. Snow (type 1) always hits this gate.

2. **No sprite quads in init:** The snow path in `InitializeWeatherOverlayParticles` allocates buffers and seeds random positions but **never calls `BuildSpriteQuadTemplate`**. Even if the render gate were bypassed, particles would have no renderable geometry.

3. **No UV extraction in init:** The snow path looks up "SNOWDROP" via `FindArchiveEntryByName` but **never reads the UV coordinates** from the archive entry (offsets +0x2c through +0x3c). The rain path extracts 5 UV/format values; the snow path extracts zero.

### What Would Be Needed to Restore Snow

**Minimum viable restoration (rain-style snow):**

1. **Patch the JNZ gate:** Change `0F 85 67 04 00 00` at 0x4465F4 to `90 90 90 90 90 90` (6x NOP), OR change the `CMP EAX, EBX` to always-equal. This lets type 1 fall through into the rain renderer.

2. **Add `BuildSpriteQuadTemplate` calls in snow init:** Mirror the rain path's UV extraction and quad template building for each of the 128 snow particles. Without this, even with the gate removed, particles have no geometry to render.

3. **Verify SNOWDROP sprite works:** The sprite EXISTS in `static.zip/static.hed` (at offset 0x688). It appears to be a small ~4x4 or 8x8 pixel square (vs RAINDROP's ~16x32 tall streak). The sprite resource is present but was never loaded at runtime since the init code doesn't set up the template.

**Proper snow restoration (different physics):**

The snow init seeds particles with different ranges than rain -- wider spread (16000 vs 8000 on X), higher altitude (200-8143 vs -1947-1947 on Z). A proper snow renderer would need:
- Slower fall speed (modify the wind Y constant from -250.0 to something gentler like -30.0)
- Horizontal drift (nonzero wind X, perhaps oscillating)
- Billboard quads instead of directional streaks (SNOWDROP is square, not elongated)
- No velocity-based streak elongation (snowflakes don't streak)

**Audio:** Rain.wav is loaded for all weather types by `LoadRaceAmbientSoundBuffers` (0x441C60) but only played when `weather_type == 0` (gated in `UpdateVehicleAudioMix` at ~0x4413D1). No Snow.wav exists in the sound table at 0x474A00.

---

## SpawnAmbientParticleStreak (0x42A6B0)

**Signature:** `char* __cdecl SpawnAmbientParticleStreak(int seed_value, int view_index)`

This function allocates rain splash particles in the **general race particle pool** (separate from the weather-only particle buffers). It is part of the `UpdateRaceParticleEffects` system documented in `visual-effects-system.md`.

### General Particle Pool Structure

- **Pool base:** `DAT_004a318f` (per-view, stride 0x1900 = 6400 bytes per view)
- **Slots:** 100 per view (0-99), stride 0x40 (64 bytes per slot)
- **Companion render pool:** `DAT_004a6370` (50 render slots per view, stride 0x32)
- **Render geometry:** `DAT_004a63d8 + slot * 0xB8` (184-byte sprite quad templates)

### Allocation Logic

1. Gated on `g_audioOptionsOverlayActive == 0` (no spawning while paused)
2. Finds first free slot in view's particle bank (flag byte == 0)
3. Finds first free render slot in companion array
4. Links particle slot to render slot
5. Seeds random position using the actor's world position as a seed:
   - `seed = (actor_pos >> 13) ^ (actor_pos >> 31) - (actor_pos >> 31)` (pseudo-random from position)
6. Sets particle callbacks: update at `LAB_0042a530`, render at `LAB_0042a590`
7. Sets particle flag to `0x80 | 0x60 = 0xE0` (active + specific blend modes)
8. Assigns random velocity, lifetime, and size based on seed

These splash particles use the **RAINSPL** sprite (rain splash, loaded by `InitializeRaceParticleSystem` at 0x429510) and appear as ground-level splash effects near the camera.

---

## Key Globals Summary

| Address | Name | Type | Description |
|---------|------|------|-------------|
| 0x4AEE20 | `gTrackEnvironmentConfig` | ptr | Pointer to loaded LEVELINF.DAT buffer |
| 0x4C3DE8 | `DAT_004c3de8` | int | Weather type: 0=rain, 1=snow(cut), 2=clear |
| 0x4C3DAC | `DAT_004c3dac` | ptr | Weather particle buffer, view 0 |
| 0x4C3DB0 | `DAT_004c3db0` | ptr | Weather particle buffer, view 1 |
| 0x4C3DA8 | `_DAT_004c3da8` | ptr | Weather sprite archive entry (RAINDROP/SNOWDROP) |
| 0x4C3DD8 | `DAT_004c3dd8` | int | Target particle density, view 0 |
| 0x4C3DDC | `DAT_004c3ddc` | int | Target particle density, view 1 |
| 0x4C3DE0 | `DAT_004c3de0` | int | Active particle count, view 0 |
| 0x4C3DE4 | `DAT_004c3de4` | int | Active particle count, view 1 |
| 0x4C3DB8 | `DAT_004c3db8` | float[3] | Previous camera position, view 0 (for streak direction) |
| 0x4C3DC4 | `DAT_004c3dc4` | float[3] | Previous camera position, view 1 |
| 0x4C3DD0 | `DAT_004c3dd0` | float | Previous sim_budget, view 0 (pause preservation) |
| 0x4C3DD4 | `DAT_004c3dd4` | float | Previous sim_budget, view 1 |

---

## Sprite Resources

| Name | Archive | Dimensions | Purpose |
|------|---------|------------|---------|
| RAINDROP | static.zip/static.hed @ 0x608 | ~16x32 px (tall thin streak) | Rain particle texture |
| SNOWDROP | static.zip/static.hed @ 0x688 | ~4x4 or 8x8 px (small square) | Snow particle texture (UNUSED) |
| RAINSPL | static.zip/static.hed @ 0x408 | ~64x128 px | Rain splash ground effect |
| SMOKE | static.zip/static.hed @ 0x3C8 | ~4x32 px | Tire smoke (shared particle system) |

---

## Track Weather Map

| Level ZIP | Weather | Density Pairs | Notes |
|-----------|---------|---------------|-------|
| level001 | 0 (Rain) | 6 | Full rain with gradual ramp |
| level003 | **1 (Snow)** | 2 | CUT: seg 2300->density 0, seg 2400->density 128 |
| level023 | 0 (Rain) | 6 | Rain + fog |
| level030 | 0 (Rain) | 6 | Rain + smoke |
| All others | 2 (Clear) | 0 | No weather particles |

Only **level003** has weather_type=1 (snow). It is the only track that would have shown snow if the feature were complete.

---

## Connection to Other Systems

- **Translucent render queue:** Rain quads are submitted via `QueueTranslucentPrimitiveBatch` and depth-sorted by `FlushQueuedTranslucentPrimitives` (0x431340, 510 max batches)
- **Race particle pool:** Rain splashes share the general pool with tire smoke and other effects (`UpdateRaceParticleEffects` at 0x429790, 200 total slots across 2 views)
- **Split-screen:** Fully supported. Two independent particle buffers, density trackers, and camera position histories. Each viewport renders its own rain independently.
- **Pause handling:** `g_audioOptionsOverlayActive` freezes particle positions and suppresses new spawns
- **Sub-tick interpolation:** Uses `g_subTickFraction` (0x4AAF60) for smooth camera-relative particle motion at any framerate
- **Audio:** Rain ambient sound (DXSound buffer slot 0x12) volume is modulated by particle density, gated on `weather_type == 0`

---

## No Other Weather Modes

Beyond the three types defined in LEVELINF.DAT (0=rain, 1=snow, 2=clear), there are **no other weather modes** anywhere in the codebase. No fog-as-weather, no wind effects, no lightning, no day/night cycle. The weather system is exclusively the ambient particle overlay. Fog is a separate system controlled by `LEVELINF.DAT +0x5C` and rendered via D3D table fog in `ApplyRaceFogRenderState`.
