# LEVELINF.DAT File Format

**Source:** Packed inside each `level%03d.zip` as `levelinf.dat`
**Loaded by:** `LoadTrackRuntimeData` (0x42fb90) via `ReadArchiveEntry`
**Runtime pointer:** `gTrackEnvironmentConfig` at 0x4aee20
**Fixed size:** 100 bytes (0x64)

## Structure Layout

```c
struct LevelInfoDat {  // 100 bytes total
    /* +0x00 */ uint32_t track_type;          // 0 = point-to-point, 1 = circuit (verified objdump 2026-04-24)
    /* +0x04 */ uint32_t smoke_enable;         // 1 = enable tire smoke sprites, 0 = disabled
    /* +0x08 */ uint32_t checkpoint_count;     // Number of checkpoint stages (1-7)
    /* +0x0c */ uint32_t checkpoint_spans[7];  // Span index for each checkpoint (0-padded)
    /* +0x28 */ uint32_t weather_type;         // 0 = rain, 1 = snow (CUT), 2 = clear
    /* +0x2c */ uint32_t density_pair_count;   // Number of (segment, density) pairs (0-6)
    /* +0x30 */ uint32_t is_circuit;           // 1 = circuit (lapped), 0 = point-to-point
    /* +0x34 */ struct {                       // Variable-length density pair array
                    int16_t segment_id;        // Track segment index triggering density change
                    int16_t density;           // Target particle count (clamped to 0x80=128 max)
                } density_pairs[6];            // Max 6 pairs, occupies +0x34 through +0x4b
    /* +0x4c */ uint8_t  padding[8];           // Zero padding
    /* +0x54 */ uint32_t sky_animation_index;  // 36 for circuits, 0xFFFFFFFF for P2P -- VESTIGIAL: no PC code reads this field
    /* +0x58 */ uint32_t total_span_count;     // Total spans in track (9999 sentinel for P2P)
    /* +0x5c */ uint32_t fog_enabled;          // 0 = no fog, 1 = fog active
    /* +0x60 */ uint8_t  fog_color_r;          // Fog red component (0-255)
    /* +0x61 */ uint8_t  fog_color_g;          // Fog green component (0-255)
    /* +0x62 */ uint8_t  fog_color_b;          // Fog blue component (0-255)
    /* +0x63 */ uint8_t  padding2;             // Zero padding to 0x64
};
```

## Field Details

### track_type (+0x00)
Read at VA 0x42ae6b. When == 1, sets `DAT_00466e94 = 1` (enables circuit-mode checkpoint tracking) and clears `DAT_004b0fa8`. **Value 1 = circuit (lapped), value 0 = point-to-point.** Verified by objdump @ `0x42AE6F mov [0x466e94], edi` with `EDI=1` and asset survey (2026-04-24): `+0x00==0` on the 12 P2P tracks (Moscow/Edinburgh/Sydney/BlueRidge/Honolulu/Tokyo/Keswick/SF/Bern/Kyoto/Washington/Munich) and `+0x00==1` on the 7 circuit tracks (Jarash/Newcastle/Maui/Courmayeur/Cheddar/Montego/Bez).

### smoke_enable (+0x04)
Read at VA 0x401410 by `InitializeRaceSmokeSpritePool`. When == 1, loads the "smoke" sprite from the texture archive and allocates per-actor smoke pool (racer_count * 0x170 bytes).

### checkpoint_count (+0x08)
Read at VA 0x40a04d by `CheckRaceCompletionState`. Drives the point-to-point race completion logic (used when `gTrackIsCircuit == 0` or `gTimeTrialModeEnabled != 0`).

### checkpoint_spans (+0x0c, 7 dwords)
Array of up to 7 track span indices marking checkpoint positions. Padded with zeros beyond `checkpoint_count`.

### weather_type (+0x28)
Read at VA 0x446245 by `InitializeWeatherOverlayParticles`. Copied to global `DAT_004c3de8`.
- **0 = Rain**: Full particle system + audio
- **1 = Snow**: Particle buffers allocated but rendering never executed (CUT FEATURE)
- **2 = Clear**: No weather particles or audio

### density_pair_count (+0x2c)
Read at VA 0x4464c8 by `UpdateAmbientParticleDensityForSegment`. Controls how many (segment, density) entries to scan.

### is_circuit (+0x30)
Read at VA 0x42ae7b. When == 0, clears `DAT_004aad8c` (disables certain race features for P2P tracks).

### density_pairs (+0x34, up to 6 entries)
Each 4-byte entry contains two int16 values:
- **segment_id** (+0x00 of pair): When the player enters this track segment, the target density changes
- **density** (+0x02 of pair): New target active particle count (clamped to 128 max at runtime)

Density pairs create zone-based weather intensity. Rain tracks typically have 6 pairs ramping from light (400) to heavy (1600) and back.

### fog_enabled (+0x5c)
Read at VA 0x42b443. When != 0 (and the "DAT_00466e98" fog-capable flag is set), the fog color and distance are passed to `ConfigureRaceFogColorAndMode` (0x40af10).

### fog_color (+0x60-0x62)
Three bytes packed into an RGB color value at VA 0x42b448-0x42b464. The assembly constructs `0xFFrrggbb` from the three bytes and passes it to the D3D fog configuration.

## Track Weather Map

| Level ZIP | Track Type | Weather | Smoke | Fog | Fog Color |
|-----------|-----------|---------|-------|-----|-----------|
| level001 | Circuit | Rain | Yes | No | - |
| level002 | Circuit | Clear | No | No | - |
| level003 | Circuit | **Snow** | No | No | - |
| level004 | Circuit | Clear | No | No | - |
| level005 | Circuit | Clear | No | No | - |
| level006 | Circuit | Clear | No | No | - |
| level013 | Circuit | Clear | No | Yes | (32,32,0) |
| level014 | Circuit | Clear | No | Yes | (16,16,16) |
| level015 | Circuit | Clear | No | No | - |
| level016 | Circuit | Clear | No | No | - |
| level017 | Circuit | Clear | No | Yes | (32,16,16) |
| level023 | Circuit | Rain | Yes | Yes | (0,0,0) |
| level025 | P2P | Clear | No | Yes | (16,16,32) |
| level026 | P2P | Clear | No | Yes | (64,16,0) |
| level027 | P2P | Clear | Yes | No | - |
| level028 | P2P | Clear | No | No | - |
| level029 | P2P | Clear | No | Yes | (8,8,8) |
| level030 | Circuit | Rain | Yes | No | - |
| level037 | P2P | Clear | No | No | - |
| level039 | P2P | Clear | No | Yes | (32,24,8) |
