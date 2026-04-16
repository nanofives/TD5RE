# Ghidra Data Types Created — 2026-03-28

Session: TD5_d3d.exe (`50589c8be1e74a2ca357bf035d019805`)
Category: `/TD5`

## Structs

### TD5_TrackProbeState (16 bytes)
Source: `actor-struct-first-128-bytes.md`

| Offset | Type   | Name              | Description                        |
|--------|--------|-------------------|------------------------------------|
| 0x00   | short  | span_index        | Current span index                 |
| 0x02   | short  | span_normalized   | Normalized position within span    |
| 0x04   | short  | span_accumulated  | Accumulated span distance          |
| 0x06   | short  | span_high_water   | High water mark for span progress  |
| 0x08   | short  | contact_vertex_A  | Contact vertex A index             |
| 0x0A   | short  | contact_vertex_B  | Contact vertex B index             |
| 0x0C   | byte   | sub_lane_index    | Sub-lane index within span         |
| 0x0D   | —      | (padding)         | 3 bytes padding to 16-byte boundary|

### TD5_StripSpan (24 bytes)
Source: `td5_level_formats.h`

| Offset | Type   | Name              | Description                          |
|--------|--------|-------------------|--------------------------------------|
| 0x00   | byte   | span_type         | Span type (0-11)                     |
| 0x01   | byte   | surface_attr      | Surface attribute (TD5_SurfaceType)  |
| 0x02   | byte   | reserved_02       | Padding / unused                     |
| 0x03   | byte   | packed_sub_span   | Low nibble=count, high nibble=h_off  |
| 0x04   | ushort | left_vertex_idx   | Left edge base vertex index          |
| 0x06   | ushort | right_vertex_idx  | Right edge base vertex index         |
| 0x08   | ushort | forward_link      | Forward link span index              |
| 0x0A   | ushort | backward_link     | Backward link span index             |
| 0x0C   | int    | origin_x          | World origin X                       |
| 0x10   | int    | origin_y          | World origin Y (vertical)            |
| 0x14   | int    | origin_z          | World origin Z                       |

### TD5_WeatherDensityZone (4 bytes)
Source: `td5_level_formats.h` — helper struct for TD5_LevelInf

| Offset | Type  | Name       | Description                             |
|--------|-------|------------|-----------------------------------------|
| 0x00   | short | segment_id | Track span index where density changes  |
| 0x02   | short | density    | Target active particle count (max 128)  |

### TD5_LevelInf (100 bytes)
Source: `td5_level_formats.h`

| Offset | Type                          | Name                 | Description                            |
|--------|-------------------------------|----------------------|----------------------------------------|
| 0x00   | uint                          | track_type           | 0=circuit, 1=point-to-point            |
| 0x04   | uint                          | smoke_enabled        | 1=tire smoke, 0=disabled               |
| 0x08   | uint                          | checkpoint_count     | Checkpoint stages (1-7)                |
| 0x0C   | uint[7]                       | checkpoint_spans     | Span index per checkpoint              |
| 0x28   | uint                          | weather_type         | TD5_WeatherType enum                   |
| 0x2C   | uint                          | density_pair_count   | Weather density zone pairs (0-6)       |
| 0x30   | uint                          | is_circuit           | 1=circuit, 0=point-to-point            |
| 0x34   | TD5_WeatherDensityZone[6]     | density_pairs        | Weather density zone pairs             |
| 0x4C   | byte[8]                       | padding_4C           | Zero padding                           |
| 0x54   | uint                          | sky_animation_index  | 36 for circuits, 0xFFFFFFFF for P2P    |
| 0x58   | uint                          | total_span_count     | Total spans in track                   |
| 0x5C   | uint                          | fog_enabled          | 0=no fog, nonzero=fog active           |
| 0x60   | byte                          | fog_color_r          | Fog red (0-255)                        |
| 0x61   | byte                          | fog_color_g          | Fog green (0-255)                      |
| 0x62   | byte                          | fog_color_b          | Fog blue (0-255)                       |
| 0x63   | byte                          | padding_63           | Alignment padding                      |

## Enums

### TD5_GameState (4 bytes)
| Value | Name                |
|-------|---------------------|
| 0     | GAMESTATE_INTRO     |
| 1     | GAMESTATE_MENU      |
| 2     | GAMESTATE_RACE      |
| 3     | GAMESTATE_BENCHMARK |

Applied at: `g_gameState` @ `0x004C3CE8`

### TD5_WeatherType (4 bytes)
| Value | Name          |
|-------|---------------|
| 0     | WEATHER_RAIN  |
| 1     | WEATHER_SNOW  |
| 2     | WEATHER_CLEAR |

### TD5_GameType (4 bytes)
| Value | Name                   |
|-------|------------------------|
| 1     | GAMETYPE_SINGLE_RACE   |
| 2     | GAMETYPE_CHAMPIONSHIP  |
| 3     | GAMETYPE_DRAG_RACE     |
| 4     | GAMETYPE_COP_CHASE     |
| 5     | GAMETYPE_PRACTICE      |
| 6     | GAMETYPE_MULTIPLAYER   |
| 7     | GAMETYPE_TIME_TRIAL    |
| 8     | GAMETYPE_SURVIVAL      |
| 9     | GAMETYPE_SPEED_TRAP    |

## Notes

- M2DX.dll session was not available (no open session found); types were created only in the TD5_d3d.exe session.
- The second TD5_d3d.exe session (`e0277ec8367e402eb705490db0bd4a5b`) was not modified.
- All structs verified at correct sizes after creation. Ghidra auto-grow behavior required post-creation trimming for `TD5_TrackProbeState` and `TD5_WeatherDensityZone`.
- `TD5_LevelInf` fields populated via `replaceAtOffset` to avoid auto-grow.
