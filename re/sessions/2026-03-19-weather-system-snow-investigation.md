# Weather System and Snow Rendering Investigation

**Date:** 2026-03-19
**Status:** RESOLVED -- Snow is a confirmed CUT FEATURE (partially implemented, never finished)

## Summary

The weather system in TD5 supports three weather types defined per-track in LEVELINF.DAT:
- **Type 0 (RAIN)**: Fully implemented with particle rendering, streak effects, and ambient audio
- **Type 1 (SNOW)**: Initialization code exists but **rendering is completely missing**
- **Type 2 (CLEAR)**: No weather particles or ambient audio

Snow rendering is conclusively a cut feature. The initialization path allocates buffers and seeds particle positions, but the rendering function explicitly skips all drawing when weather type != 0. No snow-specific rendering code, audio, or sprite quad templates were ever completed.

## Evidence

### 1. InitializeWeatherOverlayParticles (0x446240)

Reads `gTrackEnvironmentConfig + 0x28` (weather type) into global `DAT_004c3de8`.

**Rain path (type 0):**
- Allocates 2x 0x6400-byte particle buffers
- Looks up "RAINDROP" sprite via `FindArchiveEntryByName`
- Extracts UV coordinates from the sprite archive entry (+0x2c, +0x30, +0x34, +0x38, +0x3c)
- Seeds 128 particles per buffer with random positions in range X:[-4000,4000] Y:[-3000,3000] Z:[-1947,~1947]
- **Calls `BuildSpriteQuadTemplate` for each particle** -- builds renderable geometry

**Snow path (type 1):**
- Allocates 2x 0x6400-byte particle buffers
- Looks up "SNOWDROP" sprite via `FindArchiveEntryByName` (string at 0x474e78)
- **Does NOT extract UV coordinates** -- no sprite template setup
- Seeds 128 particles per buffer with wider random positions X:[-8000,8000] Y:[-8000,4000] Z:[200,~8143]
- **Does NOT call `BuildSpriteQuadTemplate`** -- particles have no renderable geometry

### 2. RenderAmbientParticleStreaks (0x446560)

The rendering function has a single guard at the top:

```
if (DAT_004c3de8 == 0) {
    // ... all rain rendering, spawning, transform, queue ...
}
return;
```

Assembly at VA 0x446584: `CMP EAX, EBX` / `JNZ 0x446a61` (function epilogue).
When weather type is 1 (snow), execution jumps directly to the function return. **No snow rendering code exists anywhere in the function.** There is no else-branch, no alternate renderer, no fallthrough.

### 3. UpdateVehicleAudioMix (0x4413d1, 0x44142d)

Rain ambient sound (Rain.wav, loaded into DXSound buffer slot 0x12) is only played when:
- `DAT_004c3de0` (particle density for current view) > 0, AND
- `DAT_004c3de8` (weather type) == 0

For snow tracks, Rain.wav is loaded into the buffer by `LoadRaceAmbientSoundBuffers` (which does not check weather type), but the audio mixer never plays it. **No Snow.wav or equivalent ambient sound exists** in the sound table at 0x474a00.

### 4. Sprite Resources

- "RAINDROP" string at 0x474e84 -- referenced by rain init
- "SNOWDROP" string at 0x474e78 (embedded in .rdata as "4CSNOWDROP" due to preceding float constant 180.0 overlapping the string address) -- referenced by snow init via `s_4CSNOWDROP + 2`
- Whether the SNOWDROP sprite actually exists in level003's texture archive is moot since `BuildSpriteQuadTemplate` is never called for snow particles

### 5. Level Data Confirmation

Only **level003** (pool index 2) has weather type 1 (snow) in its LEVELINF.DAT.

| Level | Weather | Density Pairs | Smoke | Fog |
|-------|---------|---------------|-------|-----|
| level001 | 0 (RAIN) | 6 | Yes | No |
| level003 | **1 (SNOW)** | **2** | No | No |
| level023 | 0 (RAIN) | 6 | Yes | Yes |
| level030 | 0 (RAIN) | 6 | Yes | No |
| All others | 2 (CLEAR) | 0 | Varies | Varies |

Level003's LEVELINF.DAT has 2 density pairs for snow:
- Segment 0, density 2400 (the track starts with dense snow)
- Segment 128, density 0 (snow clears out)

These density pairs ARE processed by `UpdateAmbientParticleDensityForSegment` at runtime, and particle counts ARE tracked -- they just never get rendered.

## What Would Be Needed to Restore Snow

To fully restore snow rendering, one would need to:

1. **Build sprite quad templates in init** (type 1 path): Add UV extraction from SNOWDROP archive entry and call `BuildSpriteQuadTemplate` for each particle, mirroring the rain path
2. **Add rendering branch in `RenderAmbientParticleStreaks`**: Either extend the `== 0` check to include type 1, or add a separate snow rendering branch with different physics (slower fall, wider drift, no vertical streaks)
3. **Verify SNOWDROP sprite exists** in level003's texture archive (textures.dat)
4. **Optionally add Snow.wav** ambient audio and wire it into `UpdateVehicleAudioMix`

The simplest restoration would change the `CMP EAX, EBX` / `JNZ` guard in `RenderAmbientParticleStreaks` to also render when type==1, and add the missing `BuildSpriteQuadTemplate` calls in the snow init path. However, snow would then use rain's streak rendering style (angled lines), not proper snowflake behavior.

## Related Functions

| Address | Name | Role |
|---------|------|------|
| 0x446240 | InitializeWeatherOverlayParticles | Sets up particle buffers per weather type |
| 0x446560 | RenderAmbientParticleStreaks | Per-frame particle update + render (rain only) |
| 0x4464b0 | UpdateAmbientParticleDensityForSegment | Adjusts active particle count per track segment |
| 0x42a6b0 | SpawnAmbientParticleStreak | Allocates streak particles in the general pool |
| 0x429510 | InitializeRaceParticleSystem | Inits SMOKE + RAINSPL sprites for general particles |
| 0x441c60 | LoadRaceAmbientSoundBuffers | Loads Rain.wav and all race ambient sounds |
| 0x440e00 | UpdateVehicleAudioMix | Rain sound playback gated on weather==0 |
| 0x401410 | InitializeRaceSmokeSpritePool | Smoke init gated on LEVELINF +0x04 |

## Key Globals

| Address | Name | Description |
|---------|------|-------------|
| 0x4aee20 | gTrackEnvironmentConfig | Pointer to loaded LEVELINF.DAT buffer |
| 0x4c3de8 | DAT_004c3de8 | Weather type (copied from config+0x28): 0=rain, 1=snow, 2=clear |
| 0x4c3dac | DAT_004c3dac | Weather particle buffer pointer (view 0) |
| 0x4c3db0 | DAT_004c3db0 | Weather particle buffer pointer (view 1) |
| 0x4c3dd8 | DAT_004c3dd8 | Target particle density (view 0) |
| 0x4c3ddc | DAT_004c3ddc | Target particle density (view 1) |
| 0x4c3de0 | DAT_004c3de0 | Active particle count (view 0) |
| 0x4c3de4 | DAT_004c3de4 | Active particle count (view 1) |
| 0x4c3da8 | _DAT_004c3da8 | Weather sprite archive entry pointer |
