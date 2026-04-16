# Runtime Struct & Global Ownership Pass — 2026-04-09

Goal: identify the next 3-5 runtime data models that should be treated like
`TD5_Actor`: typed, asserted, and given an explicit owner/global map.

## What Was Added In This Pass

The tree now hard-asserts layout for:

- `TD5_Actor`
- `TD5_StripSpan`
- `TD5_LevelInf`
- `TD5_CheckpointMetadata`
- `TD5_CheckpointTable`
- `TD5_NpcRacerGroup`
- `MeshResourceHeader`
- `PrimitiveCommand`
- `MeshVertex`
- `VertexNormal`
- `D3DSubmittedVertex`

These assertions make layout drift a build-time failure instead of a silent RE regression.

## Next Struct Targets

### 1. `TD5_StripHeader`

- Header: `re/include/td5_level_formats.h`
- Owner:
  - `td5_track.c`
- Why next:
  - it is the root of span-table/vertex-table relocation and a bad field here poisons the whole track path
- Next work:
  - add size and field-offset assertions
  - document every runtime global populated from it

### 2. `TD5_StaticHedHeader`

- Header: `re/include/td5_level_formats.h`
- Owner:
  - `td5_asset.c`
  - `td5_track.c`
- Why next:
  - it gates texture page counts and metadata layout for static/track texture loading
- Next work:
  - add size assertions
  - map runtime ownership of named entries and page metadata buffers

### 3. `TD5_TextureCacheEntry`

- Header: `re/include/td5_level_formats.h`
- Owner:
  - `td5_track.c`
  - render-side texture binding helpers
- Why next:
  - it is a live runtime structure, not just a file format
  - incorrect ownership here leads to texture streaming bugs that look like rendering bugs
- Next work:
  - add explicit size assertions
  - map the runtime array/global owner and write path

### 4. `TD5_CameraPreset`

- Header: `re/include/td5_level_formats.h`
- Owner:
  - `td5_camera.c`
- Why next:
  - camera presets still influence race feel heavily
  - a typed preset table should be the only source of preset semantics
- Next work:
  - assert offsets already covered by size; next step is global table ownership and callsites

### 5. `TD5_ConfigBuffer` / persisted save structures

- Source:
  - `td5_save.c`
- Owner:
  - `td5_save.c`
- Why next:
  - persistence bugs are expensive and subtle
  - save structures should be typed and offset-checked as aggressively as actor/runtime structs
- Next work:
  - inventory every persisted block and map it back to the high-level systems that consume it

## Global Ownership Targets

These globals should be assigned one canonical owner in docs and source comments:

### Race globals

- `g_td5`
- `g_actor_table_base`
- `g_actorSlotForView`
- `g_tick_counter`
- `g_subTickFraction`

### Track globals

- `s_span_table`
- `s_vertex_table`
- `s_span_count`
- level environment / checkpoint runtime buffers

### Camera globals

- `g_camWorldPos`
- `g_cameraPos`
- `g_cameraTransitionActive`

## Ownership Rule

For each runtime struct/global:

1. name the canonical owning module
2. list read-mostly consumers separately
3. prefer typed access over raw offset math once a field is credible
4. add assertions before widening usage

This is the easiest way to stop semantic drift from leaking across the codebase.
