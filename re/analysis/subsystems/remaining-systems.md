# Remaining Undocumented Systems -- Deep Dive

Analysis date: 2026-03-20
Binary: TD5_d3d.exe (port 8195)

---

## 1. Pause Menu System

### InitializePauseMenuOverlayLayout (0x43B7C0)

**Called by**: `InitializeRaceSession` (0x42b55e) -- built once at race start.

**Architecture**: Centered overlay rendered as D3D sprite quads on top of the race view. All geometry is positioned relative to a computed center offset (`DAT_004b11d4 = half_width`), making it resolution-independent.

**Menu theme table** at 0x474498: 8 entries (int), each an index into the string/asset pointer table at `PTR_s_PAUSED_004744b8`. The table stride is `0xC * param_1` (12 bytes per entry group). `param_1` is clamped to `[0..7]`.

The overlay height values per theme (from 0x474498):
```
[0] = 256,  [1] = 320,  [2] = 298,  [3] = 268
[4] = 320,  [5] = 301,  [6] = 327,  [7] = 332
```

**Sprite assets loaded from the race archive**:
| Asset      | Purpose                                    |
|------------|--------------------------------------------|
| `BLACKBOX` | Background panel (112x112 quad centered)   |
| `SELBOX`   | Selection highlight bar (256x16 strip)     |
| `SLIDER`   | Volume slider fill bar (256xN strip)       |
| `BLACKBAR` | Dark bar behind each slider row            |
| `PAUSETXT` | Bitmap font glyph sheet for menu text      |

**Menu items** (per-language, 8 entries per theme x 8 bytes each = string pointer + type flag):

The string table entries, read from memory starting at 0x4747a4, contain up to 6 items per theme. The English theme (index 0) uses:
| String         | Type | Purpose                     |
|----------------|------|-----------------------------|
| `"PAUSED"`     | 2    | Title (centered)            |
| `"VIEW"`       | 0    | Camera select (left-aligned)|
| `"MUSIC"`      | 0    | Music volume slider         |
| `"SOUND"`      | 0    | Sound volume slider         |
| `"CONTINUE"`   | 2    | Resume race (centered)      |
| `"EXIT"`       | 2    | Quit race (centered)        |

Type flag `2` = center-aligned (width measured by glyph widths from glyph table at `PTR_DAT_004660c8`). Type flag `0` = left-aligned at `4.0 - half_width`.

**Layout geometry**:
- 3 slider rows built in a loop (indices 0-2), each at Y = `i * 16.0` offset, spanning from `half_width - 131` to `half_width - 1`
- Slider fill uses the SLIDER texture, UV-mapped proportionally to the current value (0.0-1.0)
- Text labels rendered character-by-character from the PAUSETXT glyph atlas (16x16 cells, 16 columns x 16 rows), with per-glyph proportional width from the glyph width table
- Each glyph's UV is computed from `(char & 0xF) * 16` (column) and `(char >> 4) * 16` (row)
- Total quad count tracked in `DAT_004b1368`

**Localization**: At least 4 language themes observed in the string table:
- English: PAUSED, VIEW, MUSIC, SOUND, CONTINUE, EXIT
- Swedish: PAUS, VISTA, MUSIKVOLYM, SFX VOLYM, FORTSATT, AVSLUTA
- Italian: PAUSA, VOLUME MUSICA, VOLUME SFX, CONTINUA, ESCI
- (Additional themes for French, German, Spanish)

---

### RunAudioOptionsOverlay (0x43BF70)

**Called by**: `RunRaceFrame` (0x42c1de) -- called every frame while `g_audioOptionsOverlayActive != 0`.

**Architecture**: Modal in-race overlay that takes over input. Disables DXInput custom configuration to use raw keyboard navigation, then restores it on exit.

**State variables**:
| Global           | Purpose                                      |
|------------------|----------------------------------------------|
| `DAT_004b135c`   | View distance slider value (copied from 0x466ea8) |
| `DAT_004b1360`   | CD/Music volume (float 0.0-1.0, from DXSound::CDGetVolume * 1.55e-5) |
| `DAT_004b1364`   | SFX volume (float 0.0-1.0, from DXSound::GetVolume * 1.55e-5) |
| `DAT_00474640`   | Current menu cursor position (0-4), init = 3 |
| `DAT_004bcb78`   | Repeat guard (prevents held-key rapid scroll) |

**Menu items** (5 rows):
| Index | Function                    |
|-------|-----------------------------|
| 0     | View distance slider        |
| 1     | Music volume slider         |
| 2     | SFX volume slider           |
| 3     | CONTINUE (resume race)      |
| 4     | EXIT (retire from race)     |

**Input handling** (via `DXInputGetKBStick(0)`):
- Left/Right (bits 1,2): adjust current slider +/- 0.02 per frame, clamped to [0.0, 1.0]
- Up/Down (bits 9,10): move cursor with repeat guard
- Confirm (bit 18):
  - On item 3 (CONTINUE): sets `g_audioOptionsOverlayActive = 0`, calls `DXInput::SetConfiguration()` and `DXSound::UnMuteAll()`
  - On item 4 (EXIT): sets all active race slot states to retiring (state 1->2), calls `BeginRaceFadeOutTransition(0)`, writes force-feedback stop command

**Audio preview**: While cursor is on SFX row (index 2), calls `DXSound::ModifyOveride(1, 100, 0x5622, 0)` to play a preview sound. Other rows pass volume=0 (silent).

**Volume writeback**: Every frame writes back to:
- `DAT_00466ea8` = view distance
- `DAT_00474638` = music volume float
- `DAT_0047463c` = SFX volume float
- Calls `DXSound::CDSetVolume()` and `DXSound::SetVolume()` with integer-converted values

---

## 2. Cubic Spline System

### BuildCubicSpline3D (0x441F90)

**Called by**: `UpdateSplineTracksideCamera` (0x402c93) -- used exclusively for trackside replay camera smooth paths.

**Input**: 4 control points at `param_2`, each 3 ints (X, Y, Z in 8.8 fixed-point world coordinates). Total = 48 bytes.

**Output**: 15 ints at `param_1`:
```
[0..2]   = P1 (base point, copied directly from control point index 1)
[3..5]   = cubic coefficients (a) for X, Y, Z
[6..8]   = quadratic coefficients (b) for X, Y, Z
[9..11]  = linear coefficients (c) for X, Y, Z
[12..14] = constant offset (d) for X, Y, Z
```

**Coefficient matrix** at 0x474bc0 (4x4, row-major int32):
```
-1   3  -3   1
 2  -5   4  -1
-1   0   1   0
 0   2   0   0
```

This is the **Catmull-Rom spline** basis matrix (scaled by 2). The algorithm:
1. Copies control point [1] as the base position
2. Computes delta vectors: `delta[i] = (P[i] - P[1]) >> 8` for all 4 points
3. Multiplies deltas by the coefficient matrix to get cubic/quadratic/linear/constant terms per axis
4. Divides all coefficients by 2 (completing the 1/2 scale factor of the Catmull-Rom formula)

### EvaluateCubicSpline3D (0x442090)

**Called by**: `UpdateSplineTracksideCamera` (0x402cb1)

**Input**: `param_1` = output position (3 ints), `param_2` = spline coefficients (from Build), `param_3` = parameter t in 12-bit fixed-point (0 = start, 0xFFF = end)

**Evaluation** (standard cubic polynomial with 12-bit fixed-point arithmetic):
```
t2 = (t * t) >> 12
t3 = (t2 * t) >> 12
result[axis] = ((a * t3 + b * t2 + c * t) >> 12 + d) * 256 + base[axis]
```

The `* 256` restores the 8.8 fixed-point world scale.

### Usage: UpdateSplineTracksideCamera (0x402C00)

Builds a Catmull-Rom spline from 4 track waypoints (read from the strip/segment table) and smoothly interpolates the camera position along it. The parameter `t` is advanced each frame by a configurable speed stored at `DAT_00482f80[viewIndex]`, clamped to 0xFFF max. Waypoint Y coordinates have an elevation bias (`DAT_00482f90[viewIndex]`) applied.

The spline camera is one of the 11 trackside replay behaviors, providing smooth dolly-like camera movement along track segments.

---

## 3. Cross-Fade System

### CrossFade16BitSurfaces -- Variant 1 (0x40CDC0)

**Called by**: `AdvanceCrossFadeTransition` (0x40d142) -- the frontend screen transition path.

**Signature**: `CrossFade16BitSurfaces(weight, x_offset, y_offset, width, height)`

**Architecture**: MMX-accelerated alpha blend between two locked 16-bit DirectDraw surfaces, writing the result to the back buffer. Processes **4 pixels at a time** (64-bit MMX registers = 4x 16-bit pixels).

**Surfaces**:
- Source A: `DAT_00496260` (frontend surface 1)
- Source B: `DAT_00496264` (frontend surface 2)
- Destination: `DAT_00495220` (back buffer)

**Weight**: `param_1` clamped to [0, 32]. Complement = `32 - weight`.

**Pixel format handling** (two code paths based on `DAT_0049525c`):
- `0x0F` = RGB555 (15-bit): shifts by 5 and 10 for G and R channels
- `0x10` = RGB565 (16-bit): shifts by 6 and 11 for G and R channels, with green channel doubled

**MMX blend algorithm** (per 4-pixel batch):
```
For each channel (R, G, B) extracted via bitmask:
  result_channel = (srcA_channel * weight + srcB_channel * complement) >> 5
Channels are recombined with appropriate shifts.
```

The masks are set up as 4x uint16 MMX vectors:
- `0x3E0` (555 green) or `0x7C0` (565 green)
- `0x1F` (blue, both formats)
- Red extracted via shift-right past green+blue bits

**Surface locking**: Uses `IDirectDrawSurface::Lock` (vtable offset +0x64) with flags `DDLOCK_WAIT`. Error messages reference `"SNK_CrossFadeM"`. Unlock via vtable +0x80.

### CrossFade16BitSurfaces -- Variant 2 (0x40D190)

**Called by**: `UpdateExtrasGalleryDisplay` (0x40d8fa, 0x40da53) -- the extras/gallery cross-fade.

**Signature**: `CrossFade16BitSurfaces(weightA, weightB, x_offset, y_offset, IDirectDrawSurface* foreground)`

**Key differences from variant 1**:
- Takes **two independent weights** (A and B), both clamped to [0, 32]
- Accepts an explicit foreground surface pointer (`param_5`) instead of using the global `DAT_00496264`
- **Transparency masking**: When a source pixel is exactly 0x0000 (black), the output pixel is taken from the other source unblended. This is implemented via `paddusw` (saturating add) and a zero-test mask stored in MMX register slot [4]
- Additional mask constants: `0x7C0` and `0xF800` (for 565 red channel isolation)
- Error messages reference `"SNK_CrossTr"` (cross-transition)

**Usage context**: The extras gallery uses this for image-to-image cross-fades where one image may have transparent (black) regions that should show through.

### AdvanceCrossFadeTransition (0x40D120)

**Called by**: `RunFrontendConnectionBrowser` (0x419ae2) -- network browser screen transitions.

**Logic**:
1. Sets `DAT_004951dc = 1` (marks cross-fade active)
2. Calls variant 1: `CrossFade16BitSurfaces(DAT_0049522c, 0, 0, DAT_00495228, DAT_00495200)` -- weight = global frame counter, full screen rect
3. If `DAT_0049522c == 0x22` (34): transition complete
   - Clears active flag
   - Calls `BlitSecondaryFrontendRectToPrimary` to finalize
   - Sets `DAT_0048f2fc = 0` (transition done flag)
   - Returns 1 (complete)
4. Otherwise returns 0 (still in progress)

**Frame count**: 34 frames total (weight goes from 0 to 0x22). At weight 0 = 100% surface A, at weight 32 = 100% surface B, so the transition slightly overshoots (34 > 32) to ensure full completion.

**Weight curve**: The weight `DAT_0049522c` is incremented externally by the caller (likely +1 per frame), giving a **linear blend** over 34 frames. The extras gallery variant uses an exponential-decay curve (weight halved each frame) as documented in frontend-animation-system.md.

---

## 4. Projection Effect Configuration

### ConfigureActorProjectionEffect (0x40CBD0)

**Called by**: `UpdateActorTrackLightState` (0x40cdb5) -- called per-actor per-frame during race rendering.

**Purpose**: Reads the track's per-segment lighting metadata and configures one of 3 projection modes for the actor's headlight/environment effect.

**Track light table**: `DAT_004aee10` is a pointer to the track light data structure:
- `+4 + param_2*4` = segment-to-light-group index
- `+0x14 + group*4` = light type for this group

For night/tunnel tracks (`DAT_004c3d44 == 2`), the group index is offset by `DAT_004c3cf4` (night light table base).

**Actor fields used** (offsets into 0x388 actor struct):
| Offset  | Field                           |
|---------|---------------------------------|
| +0x366  | Current segment index (written) |
| +0x375  | Projection slot index (byte)    |
| +0x1CC  | Position X (int, float-cast)    |
| +0x1D0  | Position Y (int, float-cast)    |
| +0x1D4  | Position Z (int, float-cast)    |
| +0x1F4  | Heading angle (int, >>8)        |
| +0x1FC  | Velocity X (int)                |
| +0x200  | Velocity Y (int)                |
| +0x204  | Velocity Z (int)                |
| +0x20A  | Heading (short)                 |

**3 projection modes** (light type from track data):
| Type | Mode | Description                                            |
|------|------|--------------------------------------------------------|
| != 2,3 | 1 | **Static headlight**: position + heading-based offset accumulation. Uses actor position and heading angle. |
| 2    | 2    | **Heading-locked**: position + negative heading basis. Computes cos/sin of heading for directional projection. |
| 3    | 3    | **Velocity-blended**: position blended with velocity vector, scaled by `g_subTickFraction` (0x4AAF60) and a constant (0x4749D0). World-space anchor for smooth motion blur-like projection. |

### SetProjectionEffectState (0x43E210)

**Called by**: `ConfigureActorProjectionEffect` (3 call sites, one per mode).

**Signature**: `SetProjectionEffectState(slot, heading_param, position, mode, segment)`

**Slot array**: 0x20-byte entries starting at `DAT_004bf520` area (actually `DAT_004bf528 + slot * 0x20`):
| Offset | Field        | Description                    |
|--------|--------------|--------------------------------|
| +0x00  | cos/scale    | Cos(heading) or 1.0            |
| +0x04  | sin/zero     | Sin(heading) or 0.0            |
| +0x08  | accumulator  | Heading-based projection offset|
| +0x0C  | mode         | 1, 2, or 3                     |
| +0x10  | segment      | Track segment index            |
| +0x14  | world_x      | Mode 3: world anchor X         |
| +0x18  | world_y      | Mode 3: world anchor Y         |
| +0x1C  | world_z      | Mode 3: world anchor Z         |

**Mode behaviors**:
- **Mode 1**: Sets scale=1.0, sin=0.0. Accumulates `(sin*posX + cos*posZ) * constant` into the offset field. This creates a scrolling headlight cone effect along the heading direction.
- **Mode 2**: Stores `cos(heading)` and `sin(heading)` as a basis pair for directional projection. No accumulation.
- **Mode 3**: Stores the velocity-blended world position as an anchor point for environment-map-style projection.

**Trig functions**: `CosFloat12bit` / `SinFloat12bit` take 12-bit fixed-point angle input (0-4095 = 0-360 degrees).

---

## 5. Display Mode Enumeration

### BuildEnumeratedDisplayModeList (0x40B100)

**Called by**: `InitializeFrontendDisplayModeState` (0x414a90) and `ScreenLocalizationInit` (0x4270a2) -- at startup.

**Source data**: Reads from the M2DX DXDraw instance (`dd_exref`):
- `dd_exref + 0x1688` = total enumerated mode count
- `dd_exref + 0x34` = start of mode table (0x14 bytes per entry)
- Each mode entry: `+0x00` = width, `+0x04` = height, `+0x08` = bpp, `+0x10` = enabled flag

The mode table was already filtered by M2DX's `RecordDisplayModeIfUsable` (0x10008020) which rejects non-4:3 aspect ratios.

**Output**: Builds a filtered list at `DAT_0048da08`:
- `DAT_0048da08` = count of enabled modes
- `DAT_0048da14` onwards = 12 bytes per entry (width, height, bpp)
- Maximum 32 entries (address limit 0x48db94)

**Filter**: Only copies entries where the enabled flag (`+0x10`) is nonzero. The enabled flag is set by M2DX during DirectDraw enumeration based on hardware support.

### SelectConfiguredDisplayModeSlot (0x40B170)

**Called by**: `InitializeRaceSession` (0x42b47a) -- at race start to apply the selected resolution.

**Logic**: Walks the same `dd_exref` mode table, counting only enabled entries. When the counter matches `gSelectedDisplayModeOrdinal`, writes the underlying raw mode index to `dd_exref + 0x1694` (the active mode slot used by `DXDraw::ApplyDirectDrawDisplayMode`).

This is necessary because the frontend menu shows only enabled/filtered modes, while M2DX internally uses the full unfiltered mode table index.

### FormatDisplayModeOptionStrings (0x41D840)

**Signature**: `FormatDisplayModeOptionStrings(int* mode_list)` where `mode_list[0]` = count.

**Output**: Formats each mode entry as a string `"%dx%dx%d"` (e.g., "640x480x16") into a string table at `DAT_004974bc` with 0x20 (32) byte stride. Null-terminates after the last entry.

**Format string** at 0x466030: `"%dx%dx%d"` -- width x height x bpp.

---

## 6. Billboard Depth-Sort System

### Architecture Overview

A **4096-bucket radix sort** for translucent/overlay primitives that must be rendered back-to-front. The system is used for billboards (trees, signs), translucent triangle strips, and projected quad overlays.

### InitializeProjectedPrimitiveBuckets (0x43E5F0)

**Called once at startup**. Allocates:
- `DAT_004afb14`: Scratch buffer, 0x8000 bytes (32KB), 32-byte aligned
- `DAT_004afb48`: Secondary scratch, 0x400 bytes (1KB), 32-byte aligned
- `DAT_004bf6c4`: Bucket entry pool, 0x10000 bytes (64KB) -- holds up to 4096 entries at 16 bytes each
- `DAT_004bf520`: Current allocation pointer into the entry pool
- `DAT_004c36f8`: Running primitive count (reset each flush)

### Bucket Table

`DAT_004bf6c8` is the base of a **4096-entry linked-list bucket table** (one pointer per bucket). Each bucket index represents a depth slice. Primitives are inserted into buckets based on their averaged projected Z coordinate.

The **XOR with 0xFFF** reverses bucket order so that bucket 0 = farthest, bucket 4095 = nearest. This means iterating buckets 0..4095 during flush naturally produces back-to-front order.

### InsertBillboardIntoDepthSortBuckets (0x43E3B0)

**Called via function pointer** (data xref at 0x473bac -- render dispatch table).

**Input** (billboard batch descriptor at `param_1`):
| Offset | Field                        |
|--------|------------------------------|
| +0x02  | Texture handle (ushort)      |
| +0x08  | Quad count (ushort)          |
| +0x0A  | Triangle fan count (ushort)  |
| +0x0C  | Vertex data pointer          |

**Two primitive types processed**:

1. **Quads** (0x84 bytes per quad vertex block): Depth = average of 3 projected Z values at offsets +0x14, +0x40, +0x6C. Bucket = `round(Z * scale + bias)`. Type tag = 3.

2. **Triangle fans** (0xB0 bytes per fan vertex block): Depth = average of 4 projected Z values at offsets +0x14, +0x40, +0x6C, +0x98. Depth is treated as a signed integer with +0x200 bias, then shifted right by 6. Type tag = 4.

**Bucket entry** (16 bytes):
```
[0] = next pointer (linked list)
[1] = vertex data pointer
[2] = type tag (3=quad, 4=fan)
[3] = texture handle
```

Global primitive counter `DAT_004c36f8` is incremented by `quad_count + fan_count`.

**Depth scale/bias constants**:
- `DAT_004749e8` / `DAT_004749ec` for quads
- Fan depth uses raw integer sum with bias 0x200, range check < 0x40000

### InsertTriangleIntoDepthSortBuckets (0x43E4C0)

**Called by**: `EmitTranslucentTriangleStripDirect` (0x43173e).

**Single triangle**: Depth from 3 projected Z values at offsets +0x1C, +0x48, +0x74. Uses scale `DAT_004749f0` / bias `DAT_004749f4`. Type tag = `0x80000003` (bit 31 set = immediate primitive flag).

### QueueProjectedPrimitiveBucketEntry (0x43E550)

**Called by**: `EmitTranslucentQuadDirect` (0x4316de).

**Single quad**: Depth from 4 projected Z values at offsets +0x1C, +0x48, +0x74, +0xA0. Uses scale `DAT_004749f8` / bias `DAT_004749fc`. Type tag = `0x80000004` (bit 31 set).

### FlushProjectedPrimitiveBuckets (0x43E2F0)

**Called by**: `RunRaceFrame` (0x42beb1, 0x42c0fe) -- twice per frame (once per view in split-screen).

**Algorithm**:
1. Updates high-water mark: `DAT_004c36fc = max(DAT_004c36fc, DAT_004c36f8)`
2. Iterates all 4096 buckets from index 0 (farthest) to 4095 (nearest)
3. For each bucket, walks the linked list:
   - Sets `DAT_0048da04` = texture handle (for batching)
   - If type bit 31 is **clear** (batched billboard): calls `SubmitProjectedPolygon(vertex_data, type)` -- handles quads (type 3) and fans (type 4) as indexed primitives
   - If type bit 31 is **set** (immediate primitive): copies texture handle into vertex header, then:
     - Type `0x80000003`: calls `SubmitProjectedTrianglePrimitive`
     - Type `0x80000004`: calls `SubmitProjectedQuadPrimitive`
4. Clears each bucket pointer to NULL as it goes
5. Resets primitive count and allocation pointer
6. Calls `FlushImmediateDrawPrimitiveBatch()` to submit any remaining D3D draw calls

**Texture batching**: `DAT_0048da04` and `DAT_0048da00` are set to `0xFFFFFFFF` before the loop, enabling the submit functions to detect texture changes and batch consecutive same-texture primitives into single DrawPrimitive calls.

### RecomputeTracksideProjectionScale (0x43E900)

**Called by**: `UpdateTracksideCamera` (2 sites) and `UpdateSplineTracksideCamera` (2 sites) -- after camera changes.

**Purpose**: Recomputes the 3D-to-screen projection frustum parameters after the trackside camera changes its projection distance (`DAT_004c3718`).

**Computation** (for both horizontal and vertical axes):
```
half_width  = DAT_004c3700  (viewport half-width in world units)
half_height = DAT_004c3704  (viewport half-height in world units)
proj_dist   = DAT_004c3718  (projection distance / focal length)

For horizontal:
  hyp = sqrt(half_width^2 * constant + proj_dist^2)
  cos_half_fov = proj_dist / hyp
  sin_half_fov = -(half_width / (2 * hyp))

For vertical:
  hyp = sqrt(half_height^2 * constant + proj_dist^2)
  cos_half_fov = proj_dist / hyp
  sin_half_fov = -(half_height / (2 * hyp))
```

The constant at `DAT_004749c4` is a correction factor (likely 0.25 for the half-angle). The results are stored into the projection matrix slots at `DAT_004ab0a0..DAT_004ab0b8`, which are consumed by the clipping/projection pipeline during 3D rendering.

---

## 7. Frontend Presentation System

### InitializeFrontendPresentationState (0x424E40)

**Called by**: `InitializeFrontendResourcesAndState` (0x414920) -- once at game startup.

**Responsibilities**:

1. **Client origin setup**: In windowed mode (`iRam000600dc == 0`), calls `ClientToScreen()` on the app window handle to compute the screen-space offset for the frontend render target. Stores in `DAT_00497ab8` (X) and `DAT_00497abc` (Y).

2. **Initial surface clear**: Calls `FillPrimaryFrontendRect(0, 0, 0, width, height)` to black-fill, then performs 12 rounds of `Copy16BitSurfaceRect` (with both flag 0x10 and 0x11) as a surface-readiness warm-up / vsync sync.

3. **Presentation surface allocation**: Tests `dd_exref + 0xF40` bit 0x10 (hardware BltFast capability flag):
   - If set (`DAT_0049523c = 1`): direct primary surface presentation (no intermediate buffer needed)
   - If clear AND fullscreen: allocates a tracked intermediate surface via `CreateTrackedFrontendSurface(width, height)` assigned to `DAT_00495220`
   - Windowed mode always forces `DAT_0049523c = 1` (direct path)

4. **CPU feature detection**: Uses CPUID to detect:
   - CPU vendor string (stored at `DAT_00497aa0..a8`)
   - CPU version info (`DAT_00497aac`)
   - Feature flags (`DAT_00497ab0`)
   - **MMX support** (`DAT_00495234 = bit 23 of feature flags`) -- enables MMX cross-fade code paths
   - **RDTSC support** (`DAT_004951d4 = bit 4 of feature flags`) -- enables high-precision timing

5. **RDTSC calibration**: If RDTSC is available, measures TSC frequency over a `QueryPerformanceFrequency/5` interval to compute MHz rating (stored at `DAT_00497ac8`). Formatted into a string at `DAT_00497ac0`.

### PresentFrontendBufferSoftware (0x425360)

**Called by**: `RunFrontendDisplayLoop` (0x414d7c) -- the per-frame frontend presentation.

**Purpose**: Software fallback blit path. Locks both the back buffer (`DAT_00495220`) and the primary/front buffer (`dd_exref + 4`, the `IDirectDrawSurface*`), then performs a CPU-side pixel copy.

**Algorithm**:
1. Lock back buffer via vtable +0x64 (Lock), DDLOCK_WAIT
2. Lock primary surface; if DDERR_SURFACELOST, calls `Restore` (vtable +0x6C) and retries
3. Copies `DAT_00495228` (width) x `DAT_00495200` (height) pixels:
   - Source: locked back buffer data
   - Destination: primary surface + client origin offset `(DAT_00497ab8, DAT_00497abc)`
   - Copy is done 8 bytes (4 pixels) at a time in an unrolled inner loop (`width >> 2` iterations)
   - Outer loop advances by each surface's pitch (which may differ)
4. Unlocks both surfaces

**When used**: This path is active when hardware BltFast is unavailable (the `DAT_0049523c == 0` path in the display loop). It bypasses DirectDraw Blt/BltFast entirely, doing a raw Lock->memcpy->Unlock. This is the slowest path but guarantees compatibility with all DirectDraw implementations.

**Surface loss handling**: The primary surface lock has a `Restore` + retry pattern, handling Alt-Tab or mode switches gracefully.

### UpdateFrontendClientOrigin (0x425170)

**Called by**: `RunFrontendDisplayLoop` (0x414c99) -- every frame in the frontend.

**Purpose**: Refreshes the screen-space position of the game's client area. Resets `DAT_00497ab8` and `DAT_00497abc` to 0, then in windowed mode calls `ClientToScreen()` to get the current window position. In fullscreen mode, the origin stays at (0,0).

This is necessary because in windowed mode the user can drag the window, changing where the software blit or overlay positioning should target.

---

## Summary Table

| System                  | Key Insight                                                                 |
|-------------------------|-----------------------------------------------------------------------------|
| **Pause Menu**          | 8 language themes, centered overlay with BLACKBOX/SELBOX/SLIDER sprites, 5 items (view dist + 2 volumes + continue/exit) |
| **Cubic Splines**       | Catmull-Rom basis, 12-bit fixed-point parameter, used exclusively by trackside replay camera for smooth dolly paths |
| **Cross-Fade**          | MMX SIMD blend, 4 pixels/cycle, two variants (simple blend vs transparency-masked), 34-frame linear transition or exponential gallery fade |
| **Projection Effects**  | 3 modes per actor: static headlight accumulation, heading-locked directional, velocity-blended world anchor. Driven by per-segment track light metadata. |
| **Display Modes**       | DXDraw enumerates modes (pre-filtered to 4:3 by M2DX), EXE re-filters by enabled flag, max 32 entries, ordinal-to-raw-index mapping at race start |
| **Billboard Depth-Sort**| 4096-bucket linked-list radix sort, XOR-reversed for back-to-front, handles quads/fans/triangles, texture-batched flush with immediate/deferred paths |
| **Frontend Presentation**| Software Lock+memcpy fallback when no hardware BltFast, CPUID-gated MMX/RDTSC, windowed ClientToScreen tracking, surface-loss recovery |
