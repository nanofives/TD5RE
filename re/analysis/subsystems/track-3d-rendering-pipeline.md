# Track 3D Rendering Pipeline Analysis

> Date: 2026-03-19
> Binary: TD5_d3d.exe (0x400000 base)
> Scope: Full rendering path from STRIP.DAT geometry to screen pixels

## Executive Summary

The TD5 race renderer is a **software transform + D3D rasterization** pipeline. All vertex
transformation, lighting, clipping, and projection are done on the CPU. The GPU is used
only for final rasterization via `IDirect3DDevice::DrawPrimitive` (D3DPT_TRIANGLELIST,
FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, vtable offset 0x74). There is no
hardware T&L; no vertex/pixel shaders; no DrawIndexedPrimitive for world geometry.

The render order per view is:
1. Sky dome mesh
2. Track span display lists (the 3D world geometry)
3. Race actors (vehicles) + overlays + wheel billboards
4. Tire track decals
5. Ambient particle streaks
6. Smoke/particle effects
7. Translucent primitives (depth-sorted linked-list flush)
8. Projected primitives (4096-bucket depth-sorted flush)
9. HUD overlays

---

## 1. Asset Loading Pipeline

### 1.1 STRIP.DAT (Track Spline)

**Loader:** `BindTrackStripRuntimePointers` (0x444070)
**Source:** `STRIP.DAT` or `RTPIRTS.DAT` (reverse) from `level%03d.zip`

The STRIP.DAT blob has a 5-DWORD header:
```
[0] = offset to span record array (relative to blob start)
[1] = span count (gTrackSpanRingLength)
[2] = offset to vertex coordinate table
[3] = (stored to _DAT_004c3d8c)
[4] = gTrackStripAttributeCount (DAT_004c3d94)
```

After loading, runtime pointers are bound:
- `DAT_004c3d9c` = span record array base (24 bytes per span)
- `DAT_004c3d98` = vertex coordinate table (6 bytes per vertex: 3 x int16 XYZ)
- `DAT_004c3d90` = total span count

The first span record is patched to type 9 (sentinel) and the last to type 10 (sentinel),
with cross-links for wrap handling.

**Per-span 24-byte record layout:**
```
+0x00: byte   span_type          ; 1-8 = geometry types, 9/10 = sentinels
+0x01: byte   surface_attribute  ; patched by ApplyTrackStripAttributeOverrides
+0x04: uint16 left_vertex_index  ; into vertex coord table at DAT_004c3d98
+0x06: uint16 right_vertex_index
+0x0A: int16  link/next_span     ; connectivity
+0x0C: int32  origin_x
+0x14: int32  origin_z
```

### 1.2 MODELS.DAT (Track 3D Geometry)

**Loader:** `ParseModelsDat` (0x431190), called from `InitializeRaceSession` (0x42aa10)
**Source:** `MODELS.DAT` from `level%03d.zip`

Structure:
```
[0] DWORD  entry_count   -> gModelsDatEntryCount
[1..N]     8-byte entry pairs (entry_count entries):
   +0: DWORD  display_list_block_offset  (relative, gets relocated to absolute)
   +4: DWORD  (second dword, no known runtime consumer)
```

Each display list block pointed to by entry[i].dword0:
```
[0] DWORD  sub_mesh_count
[1..N] DWORD  sub_mesh_offsets (relative, relocated to absolute)
```

Each sub-mesh is a **Prepared Mesh Resource** (PMR) processed by `PrepareMeshResource`
(0x40ac00). PMR header fields at key offsets:
```
+0x04: int  command_count      ; number of primitive command records
+0x08: int  vertex_count       ; number of vertices
+0x0C: float bounding_radius   ; for frustum culling
+0x10: float world_x           ; bounding sphere center
+0x14: float world_y
+0x18: float world_z
+0x1C: float origin_x          ; render origin
+0x20: float origin_y
+0x24: float origin_z
+0x2C: ptr  command_list       ; -> array of 16-byte primitive command records
+0x30: ptr  vertex_data        ; -> vertex array (0x2C bytes per vertex)
+0x34: ptr  normal_data        ; -> vertex normals (for lighting)
```

The return value of `ParseModelsDat` is stored as `gTrackSpanRingLength` -- meaning the
entry count equals the number of track spans. **Each MODELS.DAT entry = one track span's
3D geometry.**

`GetTrackSpanDisplayListEntry(span_index)` at 0x431260 returns
`gModelsDatEntryTable[span_index * 2]` -- the display list block pointer for that span.

### 1.3 Texture System

**Track textures:**
- `LoadTrackTextureSet` (0x442670) loads `TEXTURES.DAT` + `static.hed` from the level ZIP
- `BuildTrackTextureCacheImpl` (0x40b1d0) builds the D3D texture cache:
  - Palette-indexed 64x64 tile textures decoded to ARGB via `ParseAndDecodeCompressedTrackData`
  - Mipmaps generated at multiple LOD levels (sizes from dispatch table at 0x473b70)
  - Uploaded via `DXD3DTexture::LoadRGBS24/32`

**Static textures** (car skins, sky, wheels, traffic):
- `LoadRaceTexturePages` (0x442770) loads `tpage%d.dat` from `static.zip`
- Car skins: `CARSKIN_%d.TGA` from car ZIPs, composited into 256x256 pages
- Sky: `FORWSKY.TGA` or `BACKSKY.TGA` from level ZIP
- Wheel hubs: `CARHUB_%d.TGA` from car ZIPs

**Texture streaming:**
- `AdvanceTextureStreamingScheduler` (0x40b830) implements demand-paged texture loading
- When `BindRaceTexturePage` (0x40b660) finds a page not resident, it calls the scheduler
- Pages are evicted LRU-style; max ~600 resident pages tracked at `DAT_0048dc40`
- Per-page age tracking via `AdvanceTexturePageUsageAges` (called at EndScene)

**Texture binding** (`BindRaceTexturePage` 0x40b660):
- Resolves page ID to DXD3DTexture slot
- Sets transparency mode (4 types: opaque, alpha-test, alpha-blend, wrap-clamp)
  via D3DRENDERSTATE_ALPHABLENDENABLE (0x1B), SRC/DEST blend (0x13/0x14)
- Sets texture clamp via `DXD3D::TextureClamp`
- Binds the IDirect3DTexture interface via vtable offset 0x98 (SetTexture)

### 1.4 Sky Mesh

**Source:** `sky.prr` / `SKY.PRR` from `static.zip`
**Global:** `gSkyMeshResource` (pointer to a standard PMR)

Loaded in `InitializeRaceSession`, processed with `PrepareMeshResource`. The sky dome's
texture page is hardcoded to `0x403` (set after load: `*(word*)(cmdlist + 2) = 0x403`).

Sky rotation: `AdvanceGlobalSkyRotation` (0x43d7c0) increments `DAT_004bf500` by 0x400
each non-paused tick (12-bit fixed-point angle). This only affects the global sky rotation
counter, not the sky mesh itself directly.

---

## 2. Per-Frame Render Path (RunRaceFrame 0x42b580)

### 2.1 Span Culling and Display List Assembly

After simulation ticks complete, the renderer builds a visible span list for each viewport:

1. **Compute cull window:** `gRaceTrackSpanCullWindow` is derived from the viewport layout
   configuration (`DAT_004aae40 + layoutMode * 0x40`), capped to a maximum and doubled.
   This defines the number of spans visible ahead/behind the camera.

2. **Determine start span:** For each view, the camera's current span index is read from
   the player's actor track position (`gap_0000 + 0x82`, a signed short). The start offset
   is `(currentSpan - cullWindow) / 4`, with an additional -25 span offset for time trial.

3. **Build display list array:** The loop calls `GetTrackSpanDisplayListEntry(spanIndex)`
   for each visible span, storing the resulting display list block pointer into a flat array
   at `DAT_004aac60`. Circuit tracks use modular wrap; point-to-point tracks clamp to bounds.

4. **Result:** An array of display list block pointers, one per visible span, ready for
   sequential rendering.

### 2.2 Camera Setup

Camera position is computed by either:
- `UpdateChaseCamera` (0x401590) -- 7 orbit presets, terrain-following
- `UpdateTracksideCamera` (0x402480) -- 11 replay behaviors

The camera's world position and orientation are stored in globals:
- `DAT_004aafc4/c8/cc` = camera world position
- `DAT_004aafa0..b4` = camera basis vectors (3x3 rotation matrix: right/up/forward)

### 2.3 Projection Setup

`ConfigureProjectionForViewport` (0x43e7e0) establishes the software projection:
```c
focal = width * 0.5625;   // _DAT_004c3718  (4:3 assumption: 640*0.5625=360)
inv_focal = 1.0 / focal;  // _DAT_004c371c
// Horizontal frustum half-plane normal:
h_len = sqrt(width*width*0.25 + focal*focal);
_DAT_004ab0b0 = focal / h_len;   // horizontal frustum cos
_DAT_004ab0b8 = -(width / (2*h_len));  // horizontal frustum sin
// Vertical frustum half-plane normal:
v_len = sqrt(height*height*0.25 + focal*focal);
_DAT_004ab0a4 = focal / v_len;   // vertical frustum cos
_DAT_004ab0a8 = -(height / (2*v_len)); // vertical frustum sin
```
The focal length is **width * 0.5625**, meaning the HFOV is locked and VFOV shrinks on
non-4:3 displays (vert- scaling). The hor+ widescreen fix changes this to `height * 0.75`.

### 2.4 BeginScene / EndScene

- `BeginRaceScene` (0x40ade0): calls `DXD3D::BeginScene()`, resets texture sort keys
- `EndRaceScene` (0x40ae00): calls `DXD3D::EndScene()`, then `AdvanceTexturePageUsageAges()`

---

## 3. Track Geometry Rendering

### 3.1 RenderTrackSpanDisplayList (0x431270)

This is the **core track world renderer**. Called once per visible span:

```c
void RenderTrackSpanDisplayList(int *displayListBlock) {
    int count = *displayListBlock;
    for (int i = 0; i < count; i++) {
        int meshPtr = displayListBlock[i + 1];
        if (IsBoundingSphereVisibleInCurrentFrustum(meshPtr)) {
            ApplyMeshRenderBasisFromWorldPosition(meshPtr);
            if (meshPtr->type == 1 || meshPtr->type == 2) {
                // Special rotation for water/animated surfaces
                PushRenderTransform();
                LoadRenderRotationMatrix(&DAT_004ab070);
                TransformAndQueueTranslucentMesh(meshPtr);
                PopRenderTransform();
            } else {
                TransformAndQueueTranslucentMesh(meshPtr);
            }
        }
    }
}
```

**Key steps per sub-mesh:**
1. **Frustum cull** via `IsBoundingSphereVisibleInCurrentFrustum` (0x42dca0)
2. **World-to-view basis** via `ApplyMeshRenderBasisFromWorldPosition`
3. **Transform + queue** via `TransformAndQueueTranslucentMesh` (0x43dcb0)

### 3.2 Frustum Culling

`IsBoundingSphereVisibleInCurrentFrustum` (0x42dca0) performs a **5-plane bounding sphere
test** in camera space:

1. Transform the mesh's world-space center to camera space using the camera basis
2. Test against **near plane** (z + radius > near_distance)
3. Test against **far plane** (z - radius < far_distance)
4. Test against **left/right frustum planes** (dot product with horizontal half-plane normal)
5. Test against **top/bottom frustum planes** (dot product with vertical half-plane normal)

Returns 0x80000000 if culled (invisible), nonzero if visible.

`TestMeshAgainstViewFrustum` (0x42de10) is a more detailed variant used for vehicles that
also computes the depth distance for LOD/fade.

### 3.3 Software Vertex Transform

`TransformMeshVerticesToView` (0x43dd60) applies the active 3x4 render transform matrix
(stored at `DAT_004bf6b8`, 12 floats: 3x3 rotation + 3 translation) to every vertex:

```c
for each vertex v in mesh:
    v.view_x = v.x * M[0] + v.y * M[1] + v.z * M[2] + M[9]
    v.view_y = v.x * M[3] + v.y * M[4] + v.z * M[5] + M[10]
    v.view_z = v.x * M[6] + v.y * M[7] + v.z * M[8] + M[11]
```

Vertex stride is 0x2C (44 bytes). Input XYZ at +0x00, output view-space XYZ at +0x0C.

### 3.4 Vertex Lighting

`ComputeMeshVertexLighting` (0x43ddf0) computes per-vertex diffuse intensity using a
**3-directional light model** (9 coefficients at `DAT_004ab0d0..f0`) plus ambient
(`DAT_004bf6a8`):

```c
for each vertex:
    intensity = dot(normal, light_dir_0) + dot(normal, light_dir_1) + dot(normal, light_dir_2)
    intensity = clamp(intensity + ambient, 0x40, 0xFF)
    vertex.color_index = intensity  // stored at vertex offset +0x18
```

Track lighting directions are per-segment, updated by `ApplyTrackLightingForVehicleSegment`
(0x430150) and `UpdateActiveTrackLightDirections` (0x42ce90).

### 3.5 Mesh Command List Processing

`RenderPreparedMeshResource` (0x4314b0) iterates the mesh's command list (16 bytes per
command, count at PMR+0x04). Each command is dispatched through the **translucent dispatch
table** at `0x473b9c`:

```c
void RenderPreparedMeshResource(int mesh) {
    ushort *cmd = mesh->command_list;
    for (int i = mesh->command_count; i > 0; i--) {
        dispatch_table[cmd[0]](cmd);   // opcode indexes into 7-entry table
        // After dispatch: flush accumulated vertices via DrawPrimitive if batch ready
        if (vertex_count > 0 && index_count > 0) {
            BindRaceTexturePage(current_texture);
            // Remap vertex color indices to packed ARGB via DAT_004aee68 LUT
            DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1,
                          vertex_buffer, vertex_count, index_buffer, index_count, 0xC);
        }
        cmd += 8;  // 16 bytes per command
        reset_batch();
        advance_texture();
    }
}
```

### 3.6 Translucent Dispatch Table (0x473b9c)

7 opcodes handle different primitive types:

| Opcode | Function | Purpose |
|--------|----------|---------|
| 0 | EmitTranslucentTriangleStrip | Variable-count tri-strip, clipped + projected |
| 1 | EmitTranslucentTriangleStrip | (duplicate of 0) |
| 2 | SubmitProjectedTrianglePrimitive | Single triangle, depth-sorted |
| 3 | SubmitProjectedQuadPrimitive | Single quad, depth-sorted |
| 4 | InsertBillboardIntoDepthSortBuckets | Billboard geometry (roadside objects) |
| 5 | EmitTranslucentTriangleStripDirect | Tri-strip, direct depth-sort insert |
| 6 | EmitTranslucentQuadDirect | Quad, direct depth-sort insert |

**Opcodes 0/1** are the primary track surface renderers -- they handle the bulk of the
road, barriers, scenery mesh, and tunnel geometry.

**Opcode 4** is specifically for **billboards** (trees, signs, lamp posts, roadside scenery).
These are camera-facing sprites embedded in the track mesh data. The handler inserts them
into the 4096-bucket depth sort (`DAT_004bf6c8`) for correct back-to-front rendering.
Triangles use 0x84-byte stride, quads use 0xB0-byte stride.

---

## 4. Clipping Pipeline

### 4.1 Near-Plane Clipping + Projection

`ClipAndSubmitProjectedPolygon` (0x4317f0) is the heart of the software rasterizer. It:

1. **Near-plane clip:** Tests each vertex's view-space Z against `DAT_00473bbc`. Vertices
   behind the near plane are clipped using Sutherland-Hodgman interpolation on all
   attributes (position, UV, color, Z).

2. **Perspective projection:** Surviving vertices are projected:
   ```c
   rhw = 1.0 / view_z
   screen_x = view_x * rhw * focal + center_x
   screen_y = view_y * rhw * focal + center_y
   ```

3. **Screen-space classification:** Quick check if the polygon is entirely within screen
   bounds, partially outside X bounds, or partially outside Y bounds.

4. **Screen-space clipping:** Based on classification:
   - Code 1 (X clip needed): `RenderTrackSegmentBatch` (0x4323d0) clips left/right
   - Code 2 (Y clip needed): `RenderTrackSegmentBatchVariant` (0x4326d0) clips top/bottom
   - Code 3 (both): calls both batch clippers in sequence

5. **Backface culling:** Cross-product winding test eliminates back-facing triangles.

6. **Triangle fan emission:** `AppendClippedPolygonTriangleFan` (0x432ab0) converts the
   clipped polygon into a triangle fan and appends to the shared vertex/index buffer.
   When the buffer exceeds 0x3A6 indices, an immediate `DrawPrimitive` flush occurs.

### 4.2 RenderTrackSegmentBatch (0x4323d0) -- X-Axis Clipper

Clips polygon vertices against left (`DAT_004afb20`) and right (`DAT_004afb24`) screen
bounds. Uses Sutherland-Hodgman with linear interpolation of all 8-float vertex attributes
(position, UV, color, depth). Vertex stride: 8 floats = 32 bytes.

### 4.3 RenderTrackSegmentBatchVariant (0x4326d0) -- Y-Axis Clipper

Same algorithm as the X clipper but operates on Y coordinates against top (`DAT_004afb28`)
and bottom (`DAT_004afb2c`) screen bounds.

---

## 5. Translucent Primitive Pipeline

### 5.1 Queuing

`QueueTranslucentPrimitiveBatch` (0x431460) inserts primitive command records into a
linked-list structure keyed by the sort key at record+0x02 (texture page ID). Maximum 510
batches (0x1FE). Records with the same sort key are chained together.

### 5.2 Flushing

`FlushQueuedTranslucentPrimitives` (0x431340) walks the linked list in order, dispatching
each record through the dispatch table. After processing all records, any remaining
accumulated vertices are flushed via DrawPrimitive. This ensures translucent geometry is
drawn after all opaque track geometry.

---

## 6. Projected Primitive Pipeline (Depth-Sorted)

### 6.1 Bucket Sort

`InsertBillboardIntoDepthSortBuckets` (0x43e3b0) and `QueueProjectedPrimitiveBucketEntry`
(0x43e550) insert primitives into a **4096-bucket depth sort array** at `DAT_004bf6c8`.

The bucket index is computed from the average projected Z value:
```c
bucket = (int)(avg_z * scale + bias) ^ 0xFFF;  // XOR inverts for back-to-front
```

Each bucket is a singly-linked list of 4-DWORD entries:
```
[0] next pointer
[1] primitive data pointer
[2] flags (0x3 = triangle, 0x4 = quad, 0x80000003 = projected tri, 0x80000004 = projected quad)
[3] texture page ID
```

### 6.2 Flushing

`FlushProjectedPrimitiveBuckets` (0x43e2f0) iterates all 4096 buckets in order (back to
front due to XOR inversion), processing each linked list entry:

- Flag bit 31 clear: `SubmitProjectedPolygon` -- raw polygon with explicit vertex count
- Flag == -0x7ffffffd (0x80000003): `SubmitProjectedTrianglePrimitive` -- 3-vertex
- Flag == 0x80000004: `SubmitProjectedQuadPrimitive` -- 4-vertex

All ultimately call `ClipAndSubmitProjectedPolygon` for clipping/rasterization.

After all buckets, `FlushImmediateDrawPrimitiveBatch` submits any remaining vertex data.

---

## 7. Sky Dome Rendering

The sky dome is rendered **first** in each view pass, before track geometry:

```c
ApplyMeshResourceRenderTransform(gSkyMeshResource);
TransformMeshVerticesToView(gSkyMeshResource);
SetRaceRenderStatePreset(0);   // texture filter = linear, alpha test OFF
if (fog_enabled) ApplyRaceFogRenderState(0);  // disable fog for sky
RenderPreparedMeshResource(gSkyMeshResource);
if (fog_enabled) ApplyRaceFogRenderState(1);  // re-enable fog
SetRaceRenderStatePreset(1);   // texture filter = point, alpha test ON
```

Key observations:
- Sky uses render state preset 0 (bilinear filtering, no alpha test)
- Fog is explicitly **disabled** for the sky dome, then re-enabled for track
- `AdvanceGlobalSkyRotation` rotates the dome by +0x400 per tick (12-bit fixed angle)
- Sky texture is a panoramic TGA (`FORWSKY.TGA` / `BACKSKY.TGA`)

---

## 8. Billboard / Roadside Object Rendering

Billboards (trees, signs, lamp posts) are **embedded in the track MODELS.DAT mesh data**
as opcode 4 primitive commands. They are NOT separate entities.

When `RenderPreparedMeshResource` encounters an opcode 4 command:
1. `InsertBillboardIntoDepthSortBuckets` reads triangle/quad counts from the command
2. Each billboard face is depth-sorted into the 4096-bucket array
3. They are rendered during `FlushProjectedPrimitiveBuckets`, ensuring correct depth
   ordering with other translucent geometry

Billboard animations: `AdvanceWorldBillboardAnimations` (0x43cdc0) advances an animation
phase counter by +0x10 per tick for a pool of billboard records (stride 0x22C, base
`DAT_004bedc0`), likely driving texture frame selection for animated signs/flags.

---

## 9. Vehicle Rendering

`RenderRaceActorsForView` (0x40bd20) renders all race actors:

### 9.1 Player/AI Vehicles (slots 0-5)
For each active actor slot:
1. Interpolate world position using velocity * fractional tick
2. `ApplyTrackLightingForVehicleSegment` -- set per-segment light direction
3. `ApplyMeshRenderBasisFromTransform` -- build world-to-view transform from actor matrix
4. `TestMeshAgainstViewFrustum` -- frustum cull
5. `TransformMeshVerticesToView` -- software vertex transform
6. `ComputeMeshVertexLighting` -- per-vertex diffuse lighting
7. `RenderPreparedMeshResource` -- dispatch mesh commands
8. Overlay effects: shadow quads, wheel billboards, smoke sprites, taillights

### 9.2 Traffic Vehicles (slots 6+)
Same pipeline but with **span-distance culling**: traffic actors beyond
`gRaceTrackSpanCullWindow` spans from the camera are skipped entirely.

### 9.3 Wheel Billboards
`RenderVehicleWheelBillboards` (0x446f00) builds camera-facing quad sprites for each of
the 4 wheels, incorporating steering rotation (front wheels) and spin animation (based on
wheel rotation speed). Each wheel is 8 segments of tire tread + 1 hub face, all queued as
translucent primitive batches.

---

## 10. Render State Presets

`SetRaceRenderStatePreset` (0x40b070) manages 4 D3D render state configurations:

| Preset | Filter (0x17) | Alpha Test (0x0E) | Usage |
|--------|--------------|-------------------|-------|
| 0 | 8 (linear) | OFF | Sky dome |
| 1 | 4 (point) | ON | Track geometry, main rendering |
| 2 | 8 (linear) | ON | (unused in main path) |
| 3 | 4 (point) | OFF | Projected primitive flush |

Render state 0x17 = D3DRENDERSTATE_TEXTUREMAPBLEND
Render state 0x0E = D3DRENDERSTATE_ALPHATESTENABLE

---

## 11. Fog System

`ApplyRaceFogRenderState` (0x40af50) controls Direct3D table fog:
- Mode 0: disable fog (renderstate 0x1C = 0)
- Mode 1: enable with parameters from `d3d_exref`:
  - 0x23 (fog mode) = 3 (D3DFOG_LINEAR)
  - 0x22 (fog color) from d3d_exref+0x18
  - 0x24 (fog start) from d3d_exref+0x20
  - 0x25 (fog end) from d3d_exref+0x24
  - 0x26 (fog density) from d3d_exref+0x28
  - 0x1C (fog enable) = 1

Fog is toggled per-pass: disabled for sky, enabled for track + actors + effects.
Fog availability is checked at session init via `DXD3D::CanFog()`.

---

## 12. D3D Draw Call Summary

All geometry ultimately reaches the GPU through a single draw call pattern:

```c
IDirect3DDevice->DrawPrimitive(
    D3DPT_TRIANGLELIST,        // primitive type 4
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,  // FVF 0x1C4
    vertex_buffer,             // DAT_004afb14 (shared vertex buffer)
    vertex_count,              // DAT_004afb4c
    index_buffer,              // DAT_004af314 (shared index buffer)
    index_count,               // DAT_004afb50
    0xC                        // flags
);
```

**Vertex format** (0x20 = 32 bytes per vertex):
```
+0x00: float screen_x     (pre-transformed)
+0x04: float screen_y     (pre-transformed)
+0x08: float depth_z      (for Z-buffer, modified: (1/z - 64) * scale)
+0x0C: float rhw          (1/z, reciprocal homogeneous W)
+0x10: DWORD diffuse_argb (from DAT_004aee68 color LUT, indexed by intensity byte)
+0x14: float tex_u        (perspective-corrected: u * rhw)
+0x18: float tex_v        (perspective-corrected: v * rhw)
```

The color LUT at `DAT_004aee68` (256 entries) maps intensity byte 0x00..0xFF to
packed ARGB: `(intensity - 0x01000000)` producing `0xFF000000 | (i<<16) | (i<<8) | i`
(opaque grayscale ramp).

Draw calls are batched: vertices accumulate in the shared buffer until a texture change
or buffer overflow (>0x3A6 indices) triggers a flush.

---

## 13. LOD System

TD5 uses **texture-level LOD** rather than geometric LOD:

- `ParseAndDecodeCompressedTrackData` (0x430d30) generates mipmaps at multiple resolution
  levels during track texture cache building. The mipmap pyramid sizes come from a table
  at `DAT_00473b70`.
- The texture streaming scheduler (`AdvanceTextureStreamingScheduler`) can load lower-res
  pages when GPU memory is constrained
- There is no mesh-level LOD: distant spans use the same geometry but may be entirely
  culled by the span cull window

**Span culling** acts as a coarse geometric LOD: `gRaceTrackSpanCullWindow` limits how
many spans ahead/behind are rendered. Spans outside this window are never submitted.

---

## 14. Mesh Projection Effects

`ApplyMeshProjectionEffect` (0x43dec0) applies 3 types of special UV projection:

| Mode | Description |
|------|-------------|
| 1 | Planar Y projection: UV = (x * scale + 0.5, (z + offset) * scale) -- water/ground planes |
| 2 | Rotated planar: UV rotated by sin/cos from effect params -- animated water surfaces |
| 3 | Spherical/environment map: UV from view-space normal + position -- reflections |

These are used for water surfaces, environment-mapped barriers, and similar effects.

---

## 15. Tire Track Decals

`RenderTireTrackPool` (0x43f210) manages a pool of 0x50 tire mark records (0xEC bytes each):

- Active marks age each frame; expired after 600 ticks
- After 300 ticks, marks begin fading (intensity decremented every 8th tick)
- Each mark is a textured quad, camera-transformed and submitted as a translucent batch
- Y offset of -20.0 applied to keep marks flush with the road surface

---

## 16. Complete Render Order (per view)

```
BeginRaceScene()
  |
  +-- Camera setup (chase/trackside)
  +-- AdvanceWorldBillboardAnimations()
  |
  +-- [SKY DOME]
  |   ApplyMeshResourceRenderTransform(gSkyMeshResource)
  |   TransformMeshVerticesToView(gSkyMeshResource)
  |   SetRaceRenderStatePreset(0)       // bilinear, no alpha test
  |   ApplyRaceFogRenderState(0)        // fog OFF
  |   RenderPreparedMeshResource(gSkyMeshResource)
  |   ApplyRaceFogRenderState(1)        // fog ON
  |   SetRaceRenderStatePreset(1)       // point filter, alpha test ON
  |
  +-- [TRACK GEOMETRY]
  |   for each visible span:
  |     RenderTrackSpanDisplayList(displayList)
  |       for each sub-mesh in span:
  |         IsBoundingSphereVisibleInCurrentFrustum() -> cull
  |         ApplyMeshRenderBasisFromWorldPosition()
  |         TransformAndQueueTranslucentMesh()
  |           TransformMeshVerticesToView()
  |           QueueTranslucentPrimitiveBatch() x N
  |
  +-- [VEHICLES]
  |   RenderRaceActorsForView()
  |     per actor: frustum test, transform, light, render mesh
  |     + shadows, wheels, smoke, taillights
  |
  +-- [TIRE TRACKS]
  |   RenderTireTrackPool()
  |
  +-- [AMBIENT PARTICLES]
  |   RenderAmbientParticleStreaks()
  |
  +-- [SMOKE/PARTICLES]
  |   DrawRaceParticleEffects()
  |
  +-- [TRANSLUCENT FLUSH]
  |   FlushQueuedTranslucentPrimitives()  // linked-list walk, dispatch table
  |
  +-- [PROJECTED FLUSH]
  |   SetRaceRenderStatePreset(3)         // point filter, no alpha test
  |   FlushProjectedPrimitiveBuckets()    // 4096-bucket back-to-front
  |   SetRaceRenderStatePreset(1)
  |
  +-- [HUD]
  |   DrawRaceStatusText()
  |
  (repeat for view 2 if split-screen)
  |
  +-- [HUD OVERLAY]
  |   RenderRaceHudOverlays()
  |
EndRaceScene()
```

---

## 17. Key Global Addresses

| Address | Type | Description |
|---------|------|-------------|
| `0x4c3d90` | int | Track span count |
| `0x4c3d98` | ptr | Vertex coordinate table |
| `0x4c3d9c` | ptr | Span record array (24B/span) |
| `0x4aee50` | ptr | gModelsDatEntryTable |
| `0x4aee4c` | int | gModelsDatEntryCount |
| `0x4aee54` | ptr | End of MODELS.DAT entries |
| `0x4aac60` | array | Visible span display list ptrs |
| `0x4af26c` | ptr | Translucent batch linked-list head |
| `0x4af270` | int | Translucent batch count (max 510) |
| `0x4bf6c8` | array | 4096-bucket depth sort array |
| `0x4bf520` | ptr | Depth sort work buffer cursor |
| `0x4afb14` | ptr | Shared D3D vertex buffer |
| `0x4af314` | ptr | Shared D3D index buffer |
| `0x4afb4c` | int | Accumulated vertex count |
| `0x4afb50` | int | Accumulated index count |
| `0x4c3718` | float | Projection focal length |
| `0x4c371c` | float | 1/focal |
| `0x4c3700` | float | Active viewport width |
| `0x4c3704` | float | Active viewport height |
| `0x4bf6b8` | ptr | Active 3x4 render transform matrix |
| `0x4aee68` | array | 256-entry intensity-to-ARGB color LUT |
| `0x473b9c` | ptr[7] | Translucent dispatch table |
| `0x48da00` | int | Current bound texture page |
| `0x48da04` | int | Pending texture page |
| `0x48dc34` | int | Last bound texture page (cache) |
| `0x48dc3c` | ptr | Texture cache metadata |
| `0x48dc40` | ptr | Texture streaming state |
| `0x48dc44` | ptr | Texture page data array |

---

## 18. Key Functions Summary

| Address | Name | Role |
|---------|------|------|
| 0x42b580 | RunRaceFrame | Main frame loop, drives all rendering |
| 0x42aa10 | InitializeRaceSession | Loads all race assets (MODELS.DAT, textures, sky) |
| 0x444070 | BindTrackStripRuntimePointers | Parses STRIP.DAT header |
| 0x431190 | ParseModelsDat | Relocates + prepares all track mesh resources |
| 0x40ac00 | PrepareMeshResource | Fixes up one mesh resource (relocate, UV clamp, status) |
| 0x431260 | GetTrackSpanDisplayListEntry | Maps span index to display list block |
| 0x431270 | RenderTrackSpanDisplayList | Per-span: cull + transform + queue all sub-meshes |
| 0x42dca0 | IsBoundingSphereVisibleInCurrentFrustum | 5-plane bounding sphere frustum test |
| 0x43dcb0 | TransformAndQueueTranslucentMesh | Transform vertices + queue primitive batches |
| 0x43dd60 | TransformMeshVerticesToView | 3x4 matrix * vertex position (CPU) |
| 0x43ddf0 | ComputeMeshVertexLighting | 3-light diffuse per-vertex intensity |
| 0x4314b0 | RenderPreparedMeshResource | Dispatch table walk + DrawPrimitive batching |
| 0x4317f0 | ClipAndSubmitProjectedPolygon | Near-plane clip, project, screen clip, emit |
| 0x4323d0 | RenderTrackSegmentBatch | Sutherland-Hodgman X-axis screen clipper |
| 0x4326d0 | RenderTrackSegmentBatchVariant | Sutherland-Hodgman Y-axis screen clipper |
| 0x432ab0 | AppendClippedPolygonTriangleFan | Convert clipped poly to tri-fan, append to batch |
| 0x431460 | QueueTranslucentPrimitiveBatch | Insert into sorted linked-list (max 510) |
| 0x431340 | FlushQueuedTranslucentPrimitives | Walk + dispatch linked-list |
| 0x43e3b0 | InsertBillboardIntoDepthSortBuckets | Billboard -> 4096-bucket depth sort |
| 0x43e550 | QueueProjectedPrimitiveBucketEntry | Quad -> 4096-bucket depth sort |
| 0x43e2f0 | FlushProjectedPrimitiveBuckets | Walk 4096 buckets back-to-front |
| 0x43e7e0 | ConfigureProjectionForViewport | focal = w*0.5625, frustum planes |
| 0x40b070 | SetRaceRenderStatePreset | 4 D3D state presets (filter, alpha) |
| 0x40af50 | ApplyRaceFogRenderState | D3D linear fog on/off |
| 0x40b660 | BindRaceTexturePage | Texture page bind + transparency mode |
| 0x40b830 | AdvanceTextureStreamingScheduler | Demand-paged texture loading |
| 0x40b1d0 | BuildTrackTextureCacheImpl | Decode + upload track textures |
| 0x442770 | LoadRaceTexturePages | Load all race texture pages |
| 0x43dec0 | ApplyMeshProjectionEffect | Water/env-map UV projection |
| 0x43f210 | RenderTireTrackPool | Tire track decal rendering |
| 0x446f00 | RenderVehicleWheelBillboards | Wheel sprite billboard rendering |
| 0x0040bd20 | RenderRaceActorsForView | Vehicle rendering dispatcher |
