# Car ZIP Archives & static.zip -- Complete File Inventory

Full contents and format documentation for all car ZIP archives (`cars\*.zip`),
`static.zip`, and related archives (`traffic.zip`, `environs.zip`, `SOUND\SOUND.ZIP`).

---

## Table of Contents

1. [Car ZIP Archives (cars\*.zip)](#car-zip-archives)
2. [static.zip](#staticzip)
3. [traffic.zip](#trafficzip)
4. [environs.zip](#environszip)
5. [SOUND\SOUND.ZIP](#soundzip)
6. [Related: level%03d.zip Texture Assets](#level-zip-texture-assets)

---

## Car ZIP Archives {#car-zip-archives}

### Car Inventory

There are **37 car ZIPs** in the `cars\` directory (not 10 as previously stated).
Every car ZIP has an identical file manifest of **19 files**:

| # | Car ZIP | # | Car ZIP | # | Car ZIP |
|---|---------|---|---------|---|---------|
| 1 | 128.zip | 14 | fhm.zip | 27 | sp4.zip |
| 2 | 69v.zip | 15 | frd.zip | 28 | sp5.zip |
| 3 | 97c.zip | 16 | gto.zip | 29 | sp6.zip |
| 4 | atp.zip | 17 | gtr.zip | 30 | sp7.zip |
| 5 | c21.zip | 18 | hot.zip | 31 | sp8.zip |
| 6 | cam.zip | 19 | jag.zip | 32 | ss1.zip |
| 7 | cat.zip | 20 | mus.zip | 33 | tvr.zip |
| 8 | chv.zip | 21 | nis.zip | 34 | van.zip |
| 9 | cob.zip | 22 | pit.zip | 35 | vet.zip |
| 10 | cop.zip | 23 | sky.zip | 36 | vip.zip |
| 11 | crg.zip | 24 | sp1.zip | 37 | xkr.zip |
| 12 | cud.zip | 25 | sp2.zip | | |
| 13 | day.zip | 26 | sp3.zip | | |

The car ZIP path table is at `gCarZipPathTable` (VA 0x466EEC), 37 pointers.
`gSlotCarTypeIndex` (per-race-slot) and `gExtCarIdToTypeIndex` map slot/ID to table index.

### File Manifest (all 37 ZIPs identical)

| File | Typical Size | Format | Loader Function |
|------|-------------|--------|-----------------|
| `himodel.dat` | 39-47 KB | PRR mesh (single resource) | `LoadRaceVehicleAssets` (0x443280) |
| `carparam.dat` | 268 B (0x10C) | Binary tuning+physics | `LoadRaceVehicleAssets` |
| `config.nfo` | ~140-145 B | ASCII text, 17 fields | `ScreenLocalizationInit` (0x4269D0) |
| `carskin0.tga` | ~98 KB | TGA 128x256 @ 24bpp | `LoadRaceTexturePages` (0x442770) |
| `carskin1.tga` | ~98 KB | TGA 128x256 @ 24bpp | `LoadRaceTexturePages` |
| `carskin2.tga` | ~98 KB | TGA 128x256 @ 24bpp | `LoadRaceTexturePages` |
| `carskin3.tga` | ~98 KB | TGA 128x256 @ 24bpp | `LoadRaceTexturePages` |
| `carhub0.tga` | ~5 KB | TGA 64x64 @ 8bpp (indexed) | `LoadRaceTexturePages` |
| `carhub1.tga` | ~5 KB | TGA 64x64 @ 8bpp (indexed) | `LoadRaceTexturePages` |
| `carhub2.tga` | ~5 KB | TGA 64x64 @ 8bpp (indexed) | `LoadRaceTexturePages` |
| `carhub3.tga` | ~5 KB | TGA 64x64 @ 8bpp (indexed) | `LoadRaceTexturePages` |
| `carpic0.tga` | ~229 KB | TGA 408x280 @ 16bpp | `CarSelectionScreenStateMachine` (frontend) |
| `carpic1.tga` | ~229 KB | TGA 408x280 @ 16bpp | `CarSelectionScreenStateMachine` |
| `carpic2.tga` | ~229 KB | TGA 408x280 @ 16bpp | `CarSelectionScreenStateMachine` |
| `carpic3.tga` | ~229 KB | TGA 408x280 @ 16bpp | `CarSelectionScreenStateMachine` |
| `drive.wav` | 70-126 KB | WAV (engine loop) | `LoadVehicleSoundBank` (0x443770) |
| `rev.wav` | 55-93 KB | WAV (non-local idle) | `LoadVehicleSoundBank` |
| `reverb.wav` | 45-62 KB | WAV (local player idle) | `LoadVehicleSoundBank` |
| `horn.wav` | 19-26 KB | WAV (horn) | `LoadVehicleSoundBank` |

### No Additional Files

There are **NO** low-res models, damage models, wheel models, interior models, or
shadow meshes per car. Specifically:
- **Wheel mesh**: shared `wheel.prr` in `static.zip` (all cars use the same wheel geometry)
- **Shadow**: the "shadow" sprite entry is in `static.hed` (tpage5), not per-car
- **Damage model**: when `DAT_004c3d44 == 2`, a second copy of the same `himodel.dat` is loaded with modified UV mapping (left-half only, page 0x0404) -- no separate damage mesh file
- **Interior**: none (no cockpit view)
- **LOD**: none (single detail level only)

---

### File Format Details

#### himodel.dat -- Vehicle 3D Model

Standard PRR mesh resource format. Single mesh blob (not a container).
See [3d-asset-formats.md](3d-asset-formats.md#mesh-resource-format-prr) for full PRR spec.

**Header fields (first 0x40 bytes):**
```
+0x00  uint16  render_type        (ignored on load, set by runtime)
+0x02  uint16  texture_page_id    (overwritten during load)
+0x04  int32   command_count      Sub-batch count (body panels, windows, etc.)
+0x08  int32   total_vertex_count Total vertices
+0x0C  float   bounding_radius    Bounding sphere radius
+0x10  float   center_x           (typically 0)
+0x14  float   center_y           Suspension height offset
+0x18  float   center_z           (typically 0)
+0x2C  uint32  commands_offset    Relative offset to command array
+0x30  uint32  vertices_offset    Relative offset to vertex array
+0x34  uint32  normals_offset     Relative offset to face normals (0 = none)
```

**Post-load UV patching (LoadRaceVehicleAssets):**
- Each vertex's U coordinate: `u = u * 0.5 + (slot_index & 1) * 0.5`
- Even-numbered slots (0,2,4) map to left half of skin composite (U: 0.0-0.5)
- Odd-numbered slots (1,3,5) map to right half (U: 0.5-1.0)
- Texture page assigned from `"chassis"` entry in static.hed: `(slot / 2) + chassis.texture_slot`

**Typical dimensions:**
- sp1.zip: 2 submeshes, 650 vertices, 154 tris + 47 quads = ~39 KB
- cop.zip: similar structure, ~46 KB

#### carparam.dat -- Vehicle Tuning & Physics

Fixed 0x10C (268) byte binary file, split into two sections:

```
Offset   Size   Section        Destination
0x00     0x8C   Tuning data    -> gVehicleTuningTable (35 dwords, 0x23 per slot)
0x8C     0x80   Physics data   -> gVehiclePhysicsTable (32 dwords, 0x20 per slot)
```

**Tuning section (0x8C = 140 bytes):**
- Wheel positions (front-left, front-right, rear-left, rear-right) as X/Z pairs
- Suspension hardpoints and geometry parameters
- Visual wheel offset data at +0x80..+0x88 (used by `ComputeVehicleSuspensionEnvelope`)
  - +0x82: wheel visual width (int16, scaled * 0.76171875)
  - +0x84: wheel visual radius (int16)

**Physics section (0x80 = 128 bytes):**
- Drivetrain type, gravity constant, traction, mass
- See [vehicle-dynamics-advanced.md](vehicle-dynamics-advanced.md) for field mapping

#### config.nfo -- Localized Vehicle Stat Card

ASCII text file with 17 newline-separated fields. Parsed by `sscanf` with format
`"%s\n"` x 17. Language variants exist but `config.nfo` is the default.

**Language file variants:** config.eng, config.fre, config.ger, config.ita, config.spa, config.nfo

```
Field  Index  Example (sp1.zip)          Description
-----  -----  -----------------------   -----------
  0      0    BEHOLD!_THE_MIGHTY_MAUL!  Full display name (underscores = spaces)
  1      1    MAUL                      Short name
  2      2    D                         Body type / layout code
  3      3    6                         Cylinder count / "UNKNOWN"
  4      4    $1,000,000                Price
  5      5    245/45ZR18                Front tire size
  6      6    335/40ZR18                Rear tire size
  7      7    221                       Top speed (mph)
  8      8    2.8                       0-60 time (seconds)
  9      9    99                        Weight (x100 lbs or similar unit)
 10     10    9.8                       Quarter mile time (seconds)
 11     11    O                         Drive type (B=RWD, I=FWD, O=AWD, A=other)
 12     12    9.2:1                     Compression ratio
 13     13    330/5408                  Displacement/RPM
 14     14    1.0G                      Lateral G
 15     15    545_@_4000                Torque @ RPM
 16     16    590_@_7800                Horsepower @ RPM
```

#### CARSKINn.TGA -- Vehicle Body Skin (n = 0..3)

- **Format:** TGA, 128x256, 24bpp true-color
- **Variants:** 4 per car (indices 0-3), representing color/livery variants
- **Compositing:** Two skins composited side-by-side into a 256x256 texture page:
  - Even-slot car skin fills left 128 columns
  - Odd-slot car skin fills right 128 columns
  - Copy is done in 32-pixel-wide strips with stride 0x18C between rows
- **Loader string:** `"CARSKIN%d.TGA"` (VA 0x474CA4)

#### CARHUBn.TGA -- Wheel Hub Texture (n = 0..3)

- **Format:** TGA, 64x64, 8bpp palette-indexed
- **Variants:** 4 per car, matching CARSKIN indices
- **Compositing:** Up to 6 hubs composited into a grid on the "wheels" texture page:
  - Grid layout: `column = slot % 4`, `row = slot / 4`
  - Each tile is 64x64 in a 256x128 page
  - RGB24 -> RGBA32 conversion: black pixels (R=G=B=0) get alpha=0x00, others alpha=0xFF
- **Loader string:** `"CARHUB%d.TGA"` (VA 0x474C8C)

#### CARPICn.TGA -- Car Selection Preview (n = 0..3)

- **Format:** TGA, 408x280, 16bpp
- **Used by:** Frontend car selection screen only (not loaded during race)
- **Variants:** 4 per car (matching livery/color variants)
- **Loader string:** `"CarPic%d.tga"` (VA 0x463F08)
- **Loader function:** `CarSelectionScreenStateMachine`

#### drive.wav / rev.wav / reverb.wav / horn.wav -- Vehicle Sounds

All loaded by `LoadVehicleSoundBank` (VA 0x443770) from the per-car ZIP:

| File | DXSound Slot | Loop | Purpose |
|------|-------------|------|---------|
| `drive.wav` | slot_index * 3 + 0 | Looping | Engine running sound |
| `rev.wav` or `reverb.wav` | slot_index * 3 + 1 | Looping | Idle/decel: `rev.wav` for non-local, `reverb.wav` for local player |
| `horn.wav` | slot_index * 3 + 2 | One-shot | Horn honk |

Selection logic: if `IsLocalRaceParticipantSlot(slot)` returns true, `reverb.wav` is
loaded; otherwise `rev.wav` is loaded. This gives the player car a reverb-processed
engine idle sound for immersion.

---

## static.zip {#staticzip}

### File Inventory

| File | Size | Format | Purpose |
|------|------|--------|---------|
| `sky.prr` | 50,784 B | PRR mesh (1 submesh, 1152 verts) | Sky dome 3D mesh |
| `wheel.prr` | 2,272 B | PRR mesh (1 submesh, 36 verts) | Shared wheel 3D mesh (all cars) |
| `static.hed` | 3,544 B | Binary header + named entries | Texture page directory |
| `tpage0.dat` | 262,144 B | Raw 256x256 ARGB32 (4 bpp) | Texture page 0 (car skin composite 0) |
| `tpage1.dat` | 262,144 B | Raw 256x256 ARGB32 (4 bpp) | Texture page 1 (car skin composite 1) |
| `tpage2.dat` | 262,144 B | Raw 256x256 ARGB32 (4 bpp) | Texture page 2 (car skin composite 2) |
| `environ0.tga` | 196,652 B | TGA 256x256 @ 24bpp | Default environment map texture |

**Total: 7 files.**

### static.hed -- Texture Page Directory

**Binary format:**
```
Offset          Size             Type           Description
0x00            4                int32          texture_page_count   (17 in shipped data)
0x04            4                int32          named_entry_count    (51 in shipped data)
0x08            N * 0x40         entries[N]     Named entry descriptors
0x08 + N*0x40   M * 0x10         pages[M]       Per-page metadata records
```

The runtime subtracts 4 from `texture_page_count` for level texture indexing
(last 4 slots are reserved for car skin composites). When `DAT_004c3d44 != 2`,
it subtracts an additional 4 (for traffic texture pages that may not be loaded).

#### Named Entry Descriptor (0x40 = 64 bytes)

```
Offset  Size   Type       Field
+0x00   44     char[44]   name            ASCII name, null-terminated
+0x2C   4      int32      pos_x           X position within texture page (pixels)
+0x30   4      int32      pos_y           Y position within texture page (pixels)
+0x34   4      int32      width           Sprite width (pixels)
+0x38   4      int32      height          Sprite height (pixels)
+0x3C   4      int32      texture_slot    Texture page slot index (add 0x400 at load time)
```

At load time, `LoadTrackTextureSet` adds 0x400 to each entry's `texture_slot` to
create the non-streamed texture page index. `FindArchiveEntryByName` (VA 0x442CF0)
performs case-insensitive name lookup across the entry array.

#### Complete Named Entry Catalog (51 entries)

**Pages 0-3: Car Skin Composites + Sky**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 0 | CAR0 | 0,0 | 256x256 | 0 | Car skin composite (slots 0+1) |
| 1 | CAR1 | 0,0 | 256x256 | 1 | Car skin composite (slots 2+3) |
| 2 | CAR2 | 0,0 | 256x256 | 2 | Car skin composite (slots 4+5) |
| 3 | SKY | 0,0 | 256x256 | 3 | Sky dome texture |

**Page 4: HUD/Effect Sprites (256x256 ARGB32)**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 4 | FADEYEL | 216,32 | 4x4 | 4 | Yellow fade quad |
| 5 | FADEWHT | 220,32 | 4x4 | 4 | White fade quad (screen fade overlay) |
| 6 | SPEEDO | 0,0 | 96x96 | 4 | Speedometer dial background |
| 7 | BRAKED | 0,240 | 16x16 | 4 | Brake light / taillight sprite |
| 8 | POLICELT_RED | 32,240 | 16x16 | 4 | Police light bar (red flash) |
| 9 | POLICELT_BLUE | 64,240 | 16x16 | 4 | Police light bar (blue flash) |
| 10 | POLICE_RED | 96,224 | 8x32 | 4 | Police marker billboard (red) |
| 11 | POLICE_BLUE | 104,224 | 8x32 | 4 | Police marker billboard (blue) |
| 12 | DAMAGE | 224,0 | 32x32 | 4 | Damage indicator panel (wanted mode) |
| 13 | DAMAGEB1 | 216,0 | 4x32 | 4 | Damage bar variant 1 |
| 14 | DAMAGEB2 | 220,0 | 4x32 | 4 | Damage bar variant 2 |
| 15 | SMOKE | 192,128 | 64x128 | 4 | Smoke particle sprite |
| 16 | RAINSPL | 128,0 | 64x64 | 4 | Rain splash particle sprite |
| 17 | MPH | 0,224 | 16x8 | 4 | "MPH" / "KM/H" label |
| 18 | SCANBACK | 128,224 | 32x32 | 4 | Minimap scan background tile |
| 19 | REPLAY | 160,128 | 32x32 | 4 | "REPLAY" indicator overlay |

**Page 5: Wheels, HUD Elements, Vehicle Effects (256x256 ARGB32)**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 20 | SCANDOTS | 64,200 | 24x8 | 5 | Minimap dot markers (per-racer) |
| 21 | WHEELS | 0,0 | 256x128 | 5 | Wheel hub composite grid (6 slots, 64x64 each) |
| 22 | COLOURS | 64,176 | 16x32 | 5 | Color palette for wheel tints / palette quads |
| 23 | SEMICOL | 80,176 | 16x32 | 5 | Separator/divider sprite (HUD, tire marks) |
| 24 | RAINDROP | 48,192 | 3x8 | 5 | Rain overlay particle |
| 25 | SPARK | 48,200 | 8x8 | 5 | Spark collision particle |
| 26 | SNOWDROP | 52,192 | 4x4 | 5 | Snow overlay particle (cut feature?) |
| 27 | CHASSIS | 64,128 | 64x32 | 5 | Vehicle chassis UV reference region |
| 28 | SPEEDOFONT | 96,160 | 160x32 | 5 | Speedometer digit font atlas |
| 29 | GEARNUMBERS | 128,128 | 128x16 | 5 | Gear indicator number atlas |
| 30 | POSITION | 0,208 | 192x16 | 5 | Race position indicators (1st-6th) |
| 31 | FONT | 96,192 | 160x64 | 5 | Race HUD text font glyph atlas (4x16 grid) |
| 32 | UTURN | 0,128 | 64x64 | 5 | U-turn / wrong way warning icon |
| 33 | SHADOW | 128,64 | 128x64 | 5 | Vehicle ground shadow sprite |
| 34 | INWHEEL | 0,192 | 16x16 | 5 | Inner wheel (axle/brake disc) sprite |
| 35 | NUMBERS | 0,208 | 80x48 | 5 | Lap counter / timer number atlas |

**Pages 6-11: Traffic Vehicle Skins**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 36 | TRAF1 | 0,0 | 128x128 | 6 | Traffic vehicle skin slot 1 |
| 37 | TRAF2 | 0,0 | 128x128 | 7 | Traffic vehicle skin slot 2 |
| 38 | TRAF3 | 0,0 | 128x128 | 8 | Traffic vehicle skin slot 3 |
| 39 | TRAF4 | 0,0 | 128x128 | 9 | Traffic vehicle skin slot 4 |
| 40 | TRAF5 | 0,0 | 128x128 | 10 | Traffic vehicle skin slot 5 |
| 41 | TRAF6 | 0,0 | 128x128 | 11 | Traffic vehicle skin slot 6 |

**Page 12: Pause Menu UI**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 42 | PAUSETXT | 0,32 | 256x224 | 12 | Pause menu text font/glyph atlas |
| 43 | SLIDER | 0,0 | 256x8 | 12 | Volume/option slider bar |
| 44 | BLACKBOX | 0,8 | 8x8 | 12 | Pause menu background panel |
| 45 | BLACKBAR | 8,8 | 8x8 | 12 | Pause menu option bar background |
| 46 | SELBOX | 0,16 | 256x16 | 12 | Pause menu selection highlight |

**Pages 13-16: Environment Maps**
| # | Name | Pos | Size | Page | Purpose |
|---|------|-----|------|------|---------|
| 47 | ENV1 | 0,0 | 128x128 | 13 | Environment reflection map 1 |
| 48 | ENV2 | 0,0 | 128x64 | 14 | Environment reflection map 2 |
| 49 | ENV3 | 0,0 | 128x64 | 15 | Environment reflection map 3 |
| 50 | ENV4 | 0,0 | 128x64 | 16 | Environment reflection map 4 |

#### Per-Page Metadata (0x10 = 16 bytes each, 17 records)

```
Offset  Size  Type    Field
+0x00   4     int32   transparency_flag   0=opaque(3bpp), 2=alpha(4bpp)
+0x04   4     int32   image_type          Upload mode: 0=LoadRGBS24, 1=LoadRGBS32(7), 2=LoadRGBS32(8)
+0x08   4     int32   source_width        Source texture dimension (width)
+0x0C   4     int32   source_height       Source texture dimension (height)
```

| Page | Trans | Type | Dimensions | Content |
|------|-------|------|-----------|---------|
| 0-3 | 0 | 0 | 256x256 | Car skins + Sky (opaque RGB) |
| 4-5 | 2 | 1 | 256x256 | HUD/effects (ARGB with alpha) |
| 6-11 | 0 | 0 | 128x128 | Traffic skins (opaque RGB) |
| 12 | 2 | 1 | 256x256 | Pause menu UI (ARGB with alpha) |
| 13 | 0 | 0 | 128x128 | Environment map 1 |
| 14-16 | 0 | 0 | 128x64 | Environment maps 2-4 |

### tpage%d.dat -- Raw Texture Page Data

Present in static.zip: `tpage0.dat`, `tpage1.dat`, `tpage2.dat` (3 pages for shared textures).

**Format:** Raw uncompressed pixel data.
- Opaque pages (transparency=0): RGB24, 3 bytes/pixel
- Alpha pages (transparency=2): ARGB32, 4 bytes/pixel, A first

For the 3 shipped pages (all 256x256, transparency=0,0,0 per metadata interpretation):
- `tpage0.dat`: 262,144 B = 256 * 256 * 4 -- stored as ARGB32 despite metadata saying opaque
  (this apparent inconsistency is because pages 0-2 are car skin composites that get overwritten
  by CARSKIN data at runtime; the tpage files provide a default/blank texture)

### sky.prr -- Sky Dome Mesh

Standard PRR mesh resource. Loaded from `STATIC.ZIP` (uppercase) during
`InitializeRaceSession`.

- 1 submesh, 1152 vertices
- Texture page forcibly set to 0x0403 (SKY entry slot) after load
- Sky texture itself (`FORWSKY.TGA`/`BACKSKY.TGA`) comes from the level ZIP, not static.zip

### wheel.prr -- Shared Wheel Mesh

Standard PRR mesh resource used for ALL vehicle wheel rendering.

- 1 submesh, 36 vertices
- Shared across all 37 car types -- no per-car wheel model
- Texture slot assigned from "WHEELS" named entry in static.hed
- Inner wheel face uses "INWHEEL" entry for brake disc/axle sprite

### environ0.tga -- Default Environment Map

- **Format:** TGA, 256x256 @ 24bpp
- Fallback environment reflection texture
- Additional environment maps loaded from `environs.zip` at runtime

---

## traffic.zip {#trafficzip}

### File Inventory

**31 traffic vehicle models + 31 skin textures = 62 files total.**

| File Pattern | Count | Size Range | Format |
|-------------|-------|-----------|--------|
| `model%d.prr` (0-30) | 31 | 11-22 KB | PRR mesh (standard format) |
| `skin%d.tga` (0-30) | 31 | 17,196 B each | TGA (dimensions vary by model) |

### Loader

- **Models:** `LoadRaceVehicleAssets` loads up to 6 traffic models per race, selected by
  `gNpcRacerGroupTable` index. Filename: `sprintf("model%d.prr", traffic_model_index)`.
  Each model gets texture page from `"TRAF%d"` static.hed entry.

- **Skins:** `LoadTrafficVehicleSkinTexture` (VA 0x443200) loads `skin%d.tga` from
  traffic.zip, decodes RGB24 via `DecodeArchiveImageToRgb24`, then uploads to the
  corresponding TRAF texture page.

---

## environs.zip {#environszip}

### File Inventory

**27 files -- environment/billboard TGA textures for per-track scenery objects.**

| File | Size | Format | Purpose |
|------|------|--------|---------|
| `TREE.TGA` | 9,004 B | TGA | Generic tree billboard |
| `BRIDGE.TGA` | 9,004 B | TGA | Generic bridge texture |
| `SUN.TGA` | 17,196 B | TGA | Sun/lens flare sprite |
| `MSUN.TGA` | 17,196 B | TGA | Alternate sun sprite |
| `SUNBKP.TGA` | 17,196 B | TGA | Sun backup/alternate |
| `ATRE1.TGA` | 9,004 B | TGA | Track A tree variant 1 |
| `KTRE1.TGA` | 9,004 B | TGA | Track K tree variant 1 |
| `MTRE1.TGA` | 9,004 B | TGA | Track M tree variant 1 |
| Various `*TUN*.TGA` | 9,004 B | TGA | Tunnel entrance/exit textures (per-track) |
| Various `*BRD*.TGA` | 9,004 B | TGA | Bridge texture variants (per-track) |
| Various `*BWL*.TGA` | 9,004 B | TGA | Barrier/wall textures |
| `YSHP1.TGA` | 9,004 B | TGA | Track Y shape variant |

### Loader

`LoadEnvironmentTexturePages` (VA 0x42F990) reads environment textures from
`environs.zip` using the `"ENV%d"` named entries from `static.hed` as upload targets.
The number of environment pages is stored in `*DAT_004aee10` (from level data).

In the type-2 texture mode, TGAs are decoded through `DecodeArchiveImageToRgb24` and
then uploaded via `UploadRaceTexturePage` to the ENV texture page slots.

---

## SOUND\SOUND.ZIP {#soundzip}

### File Inventory

**25 ambient/shared sound effect WAV files.**

| File | Size | Purpose |
|------|------|---------|
| `rain.wav` | 121 KB | Rain ambient loop |
| `bottom1.wav` | 75 KB | Undercarriage impact 1 |
| `bottom2.wav` | 52 KB | Undercarriage impact 2 |
| `bottom3.wav` | 53 KB | Undercarriage impact 3 |
| `bottom4.wav` | 46 KB | Undercarriage impact 4 |
| `hhit1.wav` | 43 KB | Hard collision hit 1 |
| `hhit2.wav` | 40 KB | Hard collision hit 2 |
| `hhit3.wav` | 35 KB | Hard collision hit 3 |
| `hhit4.wav` | 47 KB | Hard collision hit 4 |
| `lhit1.WAV` | 42 KB | Light collision hit 1 |
| `lhit2.WAV` | 71 KB | Light collision hit 2 |
| `lhit3.WAV` | 43 KB | Light collision hit 3 |
| `lhit4.WAV` | 69 KB | Light collision hit 4 |
| `lhit5.WAV` | 69 KB | Light collision hit 5 |
| `scrapeX.wav` | 10 KB | Scrape/grinding sound |
| `gear1.wav` | 14 KB | Gear shift sound |
| `skidbit.wav` | 41 KB | Tire skid/screech |
| `engine0.wav` - `engine5.wav` | 86 KB each | Traffic vehicle engine variants (6 types) |
| `siren3.wav` | 170 KB | Police siren (long loop) |
| `siren5.wav` | 14 KB | Police siren (short) |

### Loader

`LoadRaceAmbientSoundBuffers` (VA 0x441C50) loads all 19 shared sounds (rain through
siren) into DXSound buffer slots 0x12-0x24. The pointer table at VA 0x474A00 lists
19 filename pointers in reverse order (from siren5 down to rain).

For traffic vehicles beyond slot 5, additional engine loop buffers are loaded from
`engine%d.wav` based on the traffic vehicle variant type returned by
`GetTrafficVehicleVariantType`.

---

## Related: level%03d.zip Texture Assets {#level-zip-texture-assets}

Level ZIPs contain these texture-related files used by the texture pipeline:

| File | Format | Loader |
|------|--------|--------|
| `FORWSKY.TGA` | TGA 256x256 @ 24bpp | `LoadRaceTexturePages` -- forward sky panorama |
| `BACKSKY.TGA` | TGA 256x256 @ 24bpp | `LoadRaceTexturePages` -- reverse sky panorama |
| `TEXTURES.DAT` | Custom binary (64x64 indexed textures) | `LoadTrackTextureSet` |

The sky TGA is selected based on `gReverseTrackDirection`:
- Direction 0 (forward): `FORWSKY.TGA`
- Direction != 0 (reverse): `BACKSKY.TGA`

---

## Cross-Reference: FindArchiveEntryByName Consumers

All 46 call sites to `FindArchiveEntryByName` (VA 0x442CF0) and the entry names they look up:

| Caller Function | Entry Name(s) |
|----------------|---------------|
| `LoadRaceVehicleAssets` | "chassis", "car0", "TRAF%d" |
| `LoadRaceTexturePages` | "CAR%d", "wheels", "TRAF%d" |
| `InitializeVehicleShadowAndWheelSpriteTemplates` | "shadow" |
| `InitializeVehicleWheelSpriteTemplates` | "WHEELS", "INWHEEL", "COLOURS" |
| `InitializeVehicleTaillightQuadTemplates` | "BRAKED" |
| `InitializeRaceSmokeSpritePool` | "smoke" |
| `InitializeRaceHudFontGlyphAtlas` | "font" |
| `InitializeRaceParticleSystem` | "RAINSPL", "SMOKE" |
| `InitializeRaceHudLayout` | "numbers", "SEMICOL", "SPEEDO", "SPEEDOFONT", "GEARNUMBERS", "UTURN" |
| `InitializeMinimapLayout` | "scandots", "semicol", "scanback" |
| `InitializePauseMenuOverlayLayout` | "BLACKBOX", "SELBOX", "SLIDER", "BLACKBAR", "PAUSETXT" |
| `InitializeRaceOverlayResources` | "FADEWHT", "COLOURS" |
| `InitializeTrackedActorMarkerBillboards` | "PoliceLt_red", "PoliceLt_blue", "Police_red", "Police_blue" |
| `InitializeWantedHudOverlays` | "DAMAGE", "DAMAGEB1" |
| `InitializeTireTrackPool` | "SEMICOL" |
| `InitializeWeatherOverlayParticles` | "RAINDROP" (rain) or "SNOWDROP" (snow, via `s_4CSNOWDROP+2`) |
| `LoadEnvironmentTexturePages` | "ENV%d" |
| `SpawnVehicleSmokeVariant` | "SMOKE" |
| `SpawnVehicleSmokeSprite` | "SMOKE" |
| `SpawnVehicleSmokePuffAtPoint` | "SMOKE" |
| `SpawnVehicleSmokePuffFromHardpoint` | "SMOKE" |
