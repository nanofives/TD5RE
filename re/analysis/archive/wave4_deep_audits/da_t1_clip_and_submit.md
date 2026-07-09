# DA-T1 Deep Audit: ClipAndSubmitProjectedPolygon (0x004317F0, 3030B)

**Date:** 2026-05-22
**Auditor:** DA-T1 (deep audit subagent)
**Status:** ARCH-DIVERGENCE class (Phase 5(d) D3D Pipeline) — first byte-level audit
**Target:** orig `ClipAndSubmitProjectedPolygon` @ 0x004317F0 vs port `clip_and_submit_polygon` @ td5_render.c:687
**Pool slot:** TD5_pool14 (read-only; pool3 was already held by another session, pool0/14 had stale sessions auto-reused)

---

## Section A — Orig Pipeline Stages

### A.0 Function entry/exit FPU state (critical)

Orig **switches FPU precision-control to 24-bit single precision** for the entire function:

```
00431817  FLDCW [0x0048db90]    ; saved CW with PC bits cleared (24-bit mantissa)
...
004323ba  FLDCW [0x0048db94]    ; restore to extended (64-bit mantissa)
```

`0x0048db90` is written at `InitializeRaceRenderGlobals` (0x40ae10) via `FSTCW` then `AND 0xFFFFFCFF` (clears PC bits 8-9 → 00 = single-precision). `0x0048db94` is the same but `OR 0x300` (sets PC bits → 11 = extended).

This means every intermediate float computation inside ClipAndSubmitProjectedPolygon — including the `1.0f/z` projection, the X/Y screen-space tests, and the perspective UV recovery — is computed in **strict 32-bit float semantics**, matching modern C float arithmetic exactly.

### A.1 Input layout

Per-primitive working buffer at `DAT_004af268` (vertex pointer), `DAT_004af278` (primitive count), `DAT_004af27c` (vertex count per primitive: 3 or 4).

**Source vertex stride = 0x2C (44 bytes).** Field offsets (per `iVar17 + N` reads):
- +0x0C : float view_x
- +0x10 : float view_y
- +0x14 : float view_z              (clipped against)
- +0x18 : int32  lighting/color     (treated as int — FILD/FISUB/FIADD)
- +0x1C : float tex_u
- +0x20 : float tex_v
- (+0x00..+0x0B and +0x24..+0x2B are pre-projection state — origin coords / normal / etc.)

**Destination vertex stride = 0x20 (32 bytes, 8 floats).** Output layout:
- [0] screen_x
- [1] screen_y
- [2] depth_z (initially `1/z`, recomputed twice during the pipeline)
- [3] (filled in tail loop, was `1/z` before)
- [4] lighting/color (stored as float-of-int via ROUND())
- [5] specular (set to 0 in tail)
- [6] tex_u (initially `u*1/z`, restored to raw u in tail)
- [7] tex_v (initially `v*1/z`, restored to raw v in tail)

### A.2 Constants (from listing/memory)

| Symbol | Address | Value | Meaning |
|---|---|---|---|
| `DAT_00473bbc` | rdata | `0x42000000 = 32.0f` | **near-clip distance** |
| `DAT_00473bc0` | rdata | `0x3D000000 = 0.03125f` | (= 1/32) used to overwrite depth on inserted clip verts |
| `DAT_00473bc4` | rdata | `0x3F800000 = 1.0f` | rhw / projection numerator |
| `DAT_00473bc8` | rdata | ptr → 0x004af290 | area-sign accumulator array base |
| `DAT_00473bcc` | rdata | `0x37802008 ≈ 1.5278e-5f` | far-range inverse (≈ 1/65479) |
| `g_audioMinDistanceEpsilon` | 0x45d5f4 | `0x3F800000 = 1.0f` | aliased as `1.0` for `z = 1.0/(1/z)` |
| `g_hudSpeedoDialNearOff` | 0x45d6c0 | `0x42800000 = 64.0f` | aliased as `near_depth_subtract` |
| `_DAT_0045d78c` | 0x45d78c | `0x3F100000 = 0.5625f` | focal-length-fraction (4:3) |
| `g_projectionDepth` | 0x4c3718 | `width * 0.5625` (runtime) | focal length in pixels |
| `g_projectionCenterOffsetX` | 0x4c3710 | runtime | screen center X |
| `g_projectionCenterOffsetY` | 0x4c3714 | runtime | screen center Y |
| `DAT_004afb20/24/28/2c` | bss | runtime | left/right/top/bottom screen-clip bounds |
| `DAT_004afb30/34` | bss | runtime | half-width / half-height (used as |centered| bound) |
| `DAT_004afb18` | bss | runtime | scratch (centered Y after projection) |
| `DAT_004af310` | bss | runtime | scratch (centered X after projection) |

### A.3 Stage 1 — Near-plane Sutherland-Hodgman clip

For each input edge `(i, i+1)`:
- Inside test (bit-trick): `( (z & 0x7FFFFFFF) - 0x42000000 | z ) & 0x80000000 == 0`. This is **true iff `z >= 32.0f` AND `z >= 0`** (the OR with raw z catches negative-z, since `z < 0` has top bit set so OR ⇒ bit-31 set ⇒ "outside").
- If vertex `i` is inside, emit it unchanged (raw copy of all 6 fields).
- If edge crosses (`i_in != j_in`), emit interpolated vertex with `t = (32.0f - z_i) / (z_j - z_i)`.
  - x, y, u, v lerped as float
  - color/lighting (offset +0x18) lerped via **FILD/FISUB/FMUL/FIADD with FISTP-style ROUND** — integer-as-float interpolation then truncate back to int (then re-cast to float for storage at output [4])
  - **Output [2] is then overwritten with `0x3D000000 = 1/32`** (constant rhw at the near plane). This is important: clip-plane vertices get a fixed rhw, not interpolated rhw.

### A.4 Stage 2 — Perspective projection + screen-bound tally

After near-clip, if `out_count >= 1` (continues only if `>= 3`):
```c
DAT_004af310 = vx * (1/z) * g_projectionDepth                  ; centered screen X
DAT_004afb18 = vy * (1/z) * g_projectionDepth                  ; centered screen Y
*pfVar18      = DAT_004af310 + g_projectionCenterOffsetX        ; final screen X (NOT negated)
pfVar18[1]    = DAT_004afb18 + g_projectionCenterOffsetY        ; final screen Y (NOT negated)
pfVar18[6]   *= (1/z)                                           ; u/z (perspective-correct intermediate)
pfVar18[7]   *= (1/z)                                           ; v/z
```

Per-vertex outside-edge code `uVar11` accumulated as 2-bit bitmask:
- bit 1 (value 2) ← `(half_X_extent - abs(centered_x)) >> 31` (set if abs(centered_x) > half_X_extent)
- bit 0 (value 1) ← `(half_Y_extent - abs(centered_y)) >> 31`

The orig switches on `uVar11 & 3` after the loop:
- 0 = all inside both axes → skip screen-axis clip
- 1 = some X outside → run **only** the X-axis SH clip (one stage inlined as the `if (uVar11 == 1)` branch, equivalent to RenderTrackSegmentBatch)
- 2 = some Y outside → run **only** the Y-axis SH clip (the `if (uVar11 == 2)` branch, equivalent to RenderTrackSegmentBatchVariant)
- 3 = some X and some Y outside → call **both** out-of-line clippers (RenderTrackSegmentBatch + …Variant)

### A.5 Stage 3 — Screen-space Sutherland-Hodgman (X then Y)

Each stage is a full SH pass against one edge (left then right for X; top then bottom for Y), with two ping-pong buffers (`pfVar1` and `pfVar13` = `DAT_004af28c` and `DAT_004af288`). Per crossing edge:
- `t = (edge_value - inVec[axis]) / (outVec[axis] - inVec[axis])`
- All vertex fields ([0]/[1]/[2]/[6]/[7]) lerped as float
- Color/lighting (offset [4]) lerped via the same integer-cast + ROUND trick
- The clipped axis ([0] for X, [1] for Y) is **clamped to the edge value** to remove rounding drift

### A.6 Stage 4 — Backface (signed-area) cull and pack

Per-vertex 4-byte signed-area accumulator at `&DAT_004af290` (pointed to by `PTR_DAT_00473bc8`). Triangle fan from vert 0:
```c
cross_i = -((vx_i - vx_0)*(vy_{i+1} - vy_0) - (vx_{i+1} - vx_0)*(vy_i - vy_0) >> 31)
```
`cross_i ∈ {0, 1}` (sign bit). Accumulator entries at index 0 and 1 are bumped, plus `piVar15[2]` is set. The follow-up loop walks the accumulator and **packs out vertices whose flag is nonzero** — i.e., culls back-facing fan triangles by skipping their second vertex. This is the **only** culling pass — there's no per-tri winding test against a global orientation. Polygons survive with whichever winding they have; degenerate edges get dropped.

### A.7 Stage 5 — Final attribute restore + depth normalize

Tail loop (only entered if final `local_80 >= 3`):
```c
v[3] = v[2];                              // save (1/z)
v[2] = 1.0f / v[2];                       // depth = z (recovered)
v[5] = 0.0f;                              // specular = 0
v[6] *= v[2];                             // u = (u/z) * z   (perspective-correct re-multiply)
v[7] *= v[2];                             // v = (v/z) * z
v[2] -= 64.0f;                            // (z - near_depth_offset)   — uses g_hudSpeedoDialNearOff=64
v[2] *= 1.5278e-5f;                       // (z-64) * (1/far_range)    — DAT_00473bcc
```

Then `AppendClippedPolygonTriangleFan(verts, local_80)` emits triangle-fan indices into the global index buffer, flushing the draw call when the buffer fills (>0x3A6).

**Note the near-depth offset is 64.0f (`g_hudSpeedoDialNearOff`), NOT 32.0f.** This is intentional — the near-clip plane is at z=32 but the depth-buffer encoding biases by 64 so z=32 ends up at depth_z = -32 * 1.5278e-5 ≈ -0.000489. The orig depth range is [-eps .. 1.0] across z ∈ [32 .. ~65543].

---

## Section B — Per-Stage Port Mapping

### B.1 FPU control word

**Port:** No FPU precision-control mode switch. `_controlfp` is set globally in `main.c:405` (RC=RDN, PC unspecified — default 64-bit extended). Builds are `-fwrapv` (per recent commit) but precision mode is whatever main.c set globally.

**Status:** ARCH-DIVERGENT. Since the orig uses 24-bit single precision **only for this function**, the port is computing every intermediate in 64-bit extended (or 32-bit if the compiler stays in xmm registers). On x86 the divergence is real and measurable; on x86-64/SSE it would be a no-op. Mingw32 x87 with default extended-precision math is the worst case for divergence.

Memory note: `reference_fpu_control_word_arch_divergence_2026-05-20.md` shipped PC=64 (extended) globally. For ClipAndSubmitProjectedPolygon the orig **explicitly disagrees with that global** and uses PC=24. The port's blanket PC=64 propagates extra precision into this function. Whether visible in the diff depends on whether the next downstream consumer (rasterizer) cares about sub-LSB delta.

### B.2 Input layout

**Port (td5_render.c:702-709):** Reads `view_x/view_y/view_z/tex_u/tex_v/lighting` from `TD5_MeshVertex`. Field types: x/y/z/u/v are `float`; lighting is `uint32_t`.

**Status:** Layout-compatible (port's `TD5_MeshVertex` doesn't need to mirror 0x2c stride since the source isn't the orig's preprojected stream — port computes view-space upstream and feeds directly). OK.

### B.3 Near-clip threshold

**Orig:** `near_z = 32.0f` (DAT_00473bbc).
**Port (td5_render.c:84,694):** `DEFAULT_NEAR_CLIP = 1.0f`. → `near_z = 1.0f`.

**Status:** **DIVERGENT** by factor of 32. See Section C/D.

### B.4 Near-clip comparison

**Orig:** vertex inside iff `z >= 32.0f` AND `z >= 0` (bit-trick handles negative-z as outside).
**Port (line 719):** `i_in = (zi > near_z)` — strictly `>`, no negative-z special case.

**Status:** Boundary mismatch at `z == near_z` (orig includes, port excludes). For negative z: port's `zi > 1.0` is `false` (correct outside). For the boundary `z == 1.0f`: port says outside, orig (with `>= 32.0f`) says outside. **In practice the threshold gap (1.0 vs 32.0) dwarfs the boundary semantics**, so the `>=` vs `>` distinction is moot once #B.3 is fixed.

### B.5 Near-clip color interpolation

**Orig (FILD/FISUB/FMUL/FIADD with ROUND):**
```
new_color_int = (int)round( (color_j_int - color_i_int) * t + color_i_int )
```
**Port (line 738):** `out_color[out_count] = in_color[i];` — snap to nearer vertex, no interpolation.

**Status:** **DIVERGENT.** Port loses color/lighting smoothness at near-clip cuts. Affects visible flicker on geometry that crosses the near plane (e.g., car body passing in front of camera).

### B.6 Near-clip depth value at inserted vertex

**Orig:** After interpolating the new vertex, `pfVar13[2] = DAT_00473bc0 = 1/32`. This is the **rhw at z=32**, i.e., 1/near_z. Sets the inserted vertex's `1/z` value to the exact rhw of the clip plane, sidestepping float-roundoff on the interpolated z.
**Port (line 735):** `out_vz[out_count] = near_z;` — stores the **z value** (not 1/z), then perspective-projection step `inv_z = 1.0f / out_vz[i]` regenerates rhw.

**Status:** Algebraically equivalent when near_z is set correctly. With port's near_z=1.0f, `inv_z = 1.0f`. With orig's near_z=32.0f, `inv_z = 0.03125f`. Independent of #B.3, this clip-plane-snap behavior is fine in port.

### B.7 Projection scale

**Orig:** `screen_x = vx * (1/z) * g_projectionDepth + g_projectionCenterOffsetX`.
With `g_projectionDepth = width * 0.5625` (from ConfigureProjectionForViewport @ 0x43e7e0).
**Port (line 753-754):** `screen_x = -vx * s_focal_length * inv_z + s_center_x` with `s_focal_length = width * 0.5625f` (line 2379).

**Status:** Scale matches (`s_focal_length` == `g_projectionDepth`). BUT port **negates** `vx` and `vy` (note leading `-`). Orig does NOT negate. The comment at line 771-774 ("Y-negation in the projection reverses winding") confirms the port author knew this was a deliberate sign flip and disabled backface culling (`CullMode=NONE`) as compensation.

**Status:** **DIVERGENT** in sign. The visible effect depends on whether the upstream view-transform also has sign discrepancies that this cancels. If view-x/view-y are already flipped relative to orig before this function, the negation here cancels them and the final screen coordinates match. If view-space matches but only projection differs, polygons render mirrored. Worth verifying with a 2-vertex Frida probe (orig vs port `view_x` at function entry).

### B.8 Perspective-correct UV intermediate

**Orig:** `pfVar18[6] = u * (1/z)`, `pfVar18[7] = v * (1/z)` during projection; restored to raw u,v in the tail (after potential screen clip).
**Port (line 759-760):** `clipped[i].tex_u = out_u[i]` (raw, not multiplied by 1/z).

**Status:** Port path bypasses the screen-space SH clip entirely (D3D11 viewport-clips for us), so the perspective intermediate is never needed. **Functionally fine** but architecturally divergent. Pre-clip UVs are passed to D3D11 which does its own perspective-correct interpolation in clip-space — equivalent result.

### B.9 Screen-space outside test

**Orig:** Per-vertex 2-bit code accumulated; switches into 0/1/2/3-axis clip stages with full SH re-clip.
**Port (line 781-796):** Per-vertex `all_left|all_right|all_top|all_bottom` accumulators — reject if all verts share an out-of-edge.

**Status:** **DIVERGENT in semantics**. Port = trivial-reject only. Orig = trivial-reject + edge re-clip. Result: port submits more vertices to D3D11 (which then handles clipping at the rasterizer level). Functionally correct but perf-wasteful for off-screen geometry. No visible difference under D3D11 because the GPU clips.

### B.10 Backface / area cull

**Orig:** Per-fan-tri signed-area test, accumulator-driven pack to drop back-facing triangles within the fan. Note: orig **does not** cull the whole polygon based on a single area sign — it culls per fan-triangle, allowing one polygon to emit a mix of front and back tris.
**Port (line 765-779):** Single cross-product test on the first three verts; on `cross == 0.0f` rejects the entire polygon (degenerate). On any nonzero cross, all triangles in the fan are submitted with no further test (CullMode=NONE expected).

**Status:** **DIVERGENT but defensible.** Port relies on D3D11 to do per-tri culling and uses CullMode=NONE so neither winding is culled. Orig's per-fan-tri behavior is preserved insofar as both fronts and backs get submitted, but the orig actually performs the calculation and uses it to **filter** the fan — verifying that the back-facing fan triangles get dropped before submission. Port submits them all. **Possible visible artifact:** Z-fighting on geometry whose fan has mixed winding, since port submits twice as many triangles as orig (each fan slot gets both winds; D3D11 draws both). Without CullMode=NONE this would be wrong; with it, both render and the later-drawn one wins.

### B.11 Final depth normalization

**Orig:** `depth = (z - 64.0f) * (1/65479)` (g_hudSpeedoDialNearOff = 64, DAT_00473bcc ≈ 1/65479).
**Port (line 755):** `depth_z = z * (1.0f / 65536.0f)` — no near-subtract, near-clip-fudge mismatched.

**Status:** **DIVERGENT.** Constant gap:
- At z=32 (orig near plane): orig depth = `(32-64)/65479 = -0.000489`. Port depth = `32/65536 = 0.000488`. Sign-flipped tiny.
- At z=65535: orig depth = `(65471)/65479 ≈ 0.99988`. Port depth = `65535/65536 ≈ 0.99998`. ~0.0001 difference.
- At z=64: orig depth = `0`. Port depth = `0.000977`.

The depth-buffer divergence is small but persistent. With a D3D11 depth-buffer in [0,1], the port's slightly compressed range still works; sorting order between two surfaces with `|Δz| > 0.0001` will match. **Risk: distant flat surfaces could Z-fight** because their depth_z difference is smaller in port (0.0 to ~1.0 over [32, 65535]) than in orig (-0.0005 to ~1.0 over [32, 65535] but offset by the -64 subtract). On any depth-buffer precision concern, this is the wrong place to be subtly off.

### B.12 Outer multi-polygon loop

**Orig:** `do { ... } while (0 < DAT_004af278)` — iterates `DAT_004af278` primitives, each at `DAT_004af27c` verts. Loop lives inside ClipAndSubmitProjectedPolygon.
**Port:** Loop has been hoisted into the dispatch wrappers (`dispatch_tristrip` line 847-849 for tris, line 854-855 for quads). Each call to `clip_and_submit_polygon` handles exactly one primitive.

**Status:** Equivalent. Architectural lift, not semantic divergence.

---

## Section C — Specific Divergences (file:line ↔ orig stage)

| # | File:Line (port) | Orig site | Divergence | Class |
|---|---|---|---|---|
| 1 | td5_render.c:84  `DEFAULT_NEAR_CLIP 1.0f` | DAT_00473bbc = 32.0f | **Near-clip 32× too close** | semantic |
| 2 | td5_render.c:85  `DEFAULT_FAR_CLIP 65536.0f` | DAT_00473bcc ≈ 1/65479 | far-divisor off by 0.087% | numeric |
| 3 | td5_render.c:719 `(zi > near_z)` | bit-trick `z>=32 && z>=0` | strict `>` vs `>=` (mooted by #1 once fixed) | boundary |
| 4 | td5_render.c:738 `in_color[i]` snap | FILD/FISUB/FIADD interp + ROUND | **Color/lighting not interpolated at near-clip cut** | semantic |
| 5 | td5_render.c:753-754 `-out_vx * focal * inv_z` | `vx * inv_z * g_projectionDepth` | **Sign flip on X and Y** | semantic |
| 6 | td5_render.c:755 `vz * 1/65536` | `(vz - 64) * 1/65479` | **No -64 near-depth bias, slightly wrong divisor** | numeric |
| 7 | td5_render.c:759-760 raw `out_u` | `u * inv_z` (perspective-correct intermediate) | Defensible — port bypasses screen SH clip | architectural |
| 8 | td5_render.c:765-779 single cross test | Per-fan-tri signed-area pack | Defensible — port leans on D3D11 CullMode=NONE | architectural |
| 9 | td5_render.c:781-796 trivial-reject only | Full SH clip vs 4 screen edges (X then Y) | Defensible — D3D11 viewport-clips | architectural |
| 10 | td5_render.c:783 `0.0..viewport_width` | `DAT_004afb20..b24` (per-pass via SetProjectedClipRect) | **Port ignores split-screen sub-viewport bounds** | semantic |
| 11 | Function entry — no FLDCW | FLDCW [PC=24] at entry, FLDCW [PC=64] at exit | Port runs whole pipeline in PC=64 (whatever main.c set) | numeric |

---

## Section D — Severity Assessment

### Visible-render impact (highest first)

| # | Severity | Visible symptom |
|---|---|---|
| 1 (near=1 not 32) | **HIGH** | Polygons in z∈(1,32) range, normally clipped, are projected with huge `1/z` → screen coords blow up. Visible as: stretched/exploded geometry when camera is close to a surface (curb pop-through, dashboard near-clip), excessive subpixel snap near the camera, possible NaN/Inf if z very small. Likely cause of any "stretchy polygons" reports. |
| 5 (sign flip X/Y) | **HIGH** if not already compensated upstream | Whole scene mirrored. The fact that the port has been running suggests upstream view-transform compensates; but if anyone touches the view-transform sign, this re-emerges as a regression. Audit suggests this is a deliberate tradeoff but it is brittle — comment at port:771-774 even acknowledges this. |
| 4 (color snap at clip) | **MEDIUM** | Visible lighting/Gouraud color discontinuity on polygons crossing the near plane. Especially visible on long flat surfaces (road plane) lit with directional light. Could explain reports of "lighting popping" at the near plane. |
| 10 (sub-viewport bounds) | **MEDIUM** if/when split-screen ever used | Split-screen multiplayer would render full-viewport polygons into both player viewports. Not visible in single-player. |
| 6 (depth-bias missing) | **LOW-MEDIUM** | Subtle Z-fighting on far-distance flat geometry; depth-buffer compression is slightly off. Combined with #1 (most stuff in z<32 should have been clipped) likely amplifies Z-fighting near the camera. |
| 11 (FPU precision) | **LOW** | Sub-LSB float differences in projection. Compound with downstream physics if any render output feeds back (e.g., camera collision), but for pure render path probably invisible. |
| 8 (per-tri culling) | **LOW** | Doubles tri count submitted; perf hit, no visible bug since D3D11 handles. |
| 9 (screen SH skipped) | **LOW** | Same: perf hit, no visible bug. |
| 2 (far-divisor 1/65479 vs 1/65536) | **LOW** | 0.087% depth range compression; only matters if other systems read a depth value and compare to a hardcoded threshold. |
| 3 (`>` vs `>=`) | **NEGLIGIBLE** | Boundary case at z=near; once #1 fixed and exact-equal vertices are rare, no impact. |
| 7 (raw UV at projection) | **NEGLIGIBLE** | D3D11 perspective-correct interp handles it. |

---

## Section E — Actionable Fixes

### E.1 [HIGHEST PRIORITY] Fix near-clip threshold

**td5_render.c:84:**
```c
#define DEFAULT_NEAR_CLIP   32.0f    // was 1.0f — DAT_00473bbc
```

Audit all readers of `s_near_clip` (`project_vertex` line 453, depth-bucket sorting lines 924/937/969/981/1007, sphere-cull checks lines 1489/1550). They all use the same threshold so a single constant change cascades correctly. Risk: some content authored against `near=1` may pop out of view; expected to **improve** fidelity since orig was tuned for `near=32`.

Recommend: A/B build, run diff-race on Edinburgh AI 6-racer, measure tri count and look for stretched-geo regressions. If clean, ship.

### E.2 [HIGH] Add near-depth bias to depth normalization

**td5_render.c:755:**
```c
clipped[i].depth_z = (out_vz[i] - 64.0f) * (1.0f / 65479.0f);
```

(or use the orig constants exactly: `(z - g_hudSpeedoDialNearOff_64.0f) * 0x37802008_as_float`.) Note this lets depth_z go negative at z ∈ [32, 64) — D3D11 will clip those at the depth viewport. Combine with #E.1 (near=32, so depth ∈ [-0.000489, ~1.0]).

If you want to keep depth ∈ [0, 1] strictly, use the alternate `(z - 32.0f) * (1/65503.0f)` — but that does not match orig.

### E.3 [HIGH] Interpolate color on near-clip cut

**td5_render.c:738:**
```c
// Replace snap-to-i with per-channel int lerp matching orig FILD/FISTP semantics
uint32_t ci = in_color[i], cj = in_color[j];
uint32_t r = ((int)((cj >>  0) & 0xff) - (int)((ci >>  0) & 0xff)) * t + ((ci >>  0) & 0xff);
uint32_t g = ((int)((cj >>  8) & 0xff) - (int)((ci >>  8) & 0xff)) * t + ((ci >>  8) & 0xff);
uint32_t b = ((int)((cj >> 16) & 0xff) - (int)((ci >> 16) & 0xff)) * t + ((ci >> 16) & 0xff);
uint32_t a = ((int)((cj >> 24) & 0xff) - (int)((ci >> 24) & 0xff)) * t + ((ci >> 24) & 0xff);
out_color[out_count] = (a << 24) | (b << 16) | (g << 8) | r;  // BGRA per project convention
```

**Caveat:** Orig treats the whole 32-bit lighting word as a SINGLE `int` and lerps as `int` not 4×u8. That works because BGRA layout has the per-channel high bits at fixed positions, but only valid if neither endpoint has saturated bits that overflow into the next channel during the interp. For safety, prefer the per-channel form above. If matching orig exactly is required (e.g., for byte-faithful diff), use a single int lerp; expect a small number of color glitches on the boundary.

### E.4 [MEDIUM] Investigate X/Y sign flip in projection (#5)

This needs a runtime probe — do an orig vs port Frida snapshot of `(view_x, view_y, view_z)` at the entry to ClipAndSubmitProjectedPolygon / clip_and_submit_polygon. If the port already produces the negated view coords upstream, leave line 753-754 with the negation. Otherwise, **remove the leading `-` and re-enable CullMode=BACK**.

This is the highest-value future audit because the current "CullMode=NONE + sign flip" combination is paying a perf cost AND making backface artifacts possible (e.g., interior-of-car surfaces visible through windshield when sign-flip exposes the wrong winding).

### E.5 [DEFERRED] FPU precision-control switch

Adding `_controlfp(_PC_24, _MCW_PC)` at function entry and `_controlfp(_PC_64, _MCW_PC)` at exit would byte-match orig. Defer until a measured divergence implicates float intermediate precision. Existing memory note `reference_fpu_pc64_fwrapv_ab_measurement_2026-05-20.md` measured global PC=64 nets -8.6% field divergences — but that A/B was global, not function-local. A targeted scoped change here may improve render-path matching without rolling back physics gains.

### E.6 [DEFERRED] Per-viewport screen-clip bounds

For split-screen support, replicate `SetProjectedClipRect` semantics: a per-pass setter `s_screen_clip_l/r/t/b` populated by camera/viewport code, with the port's reject test (line 783-784) reading those instead of `0.0..viewport_width`. Until split-screen is wired up, no action needed.

---

## Audit metadata

- **Audited:** orig ClipAndSubmitProjectedPolygon 0x004317F0..0x004323C6 (3030B body)
- **Cross-referenced:** RenderTrackSegmentBatch (0x004323D0), RenderTrackSegmentBatchVariant (0x004326D0), AppendClippedPolygonTriangleFan (0x00432AB0), FlushImmediateDrawPrimitiveBatch (0x004329E0), SetProjectedClipRect (0x0043E640), SetProjectionCenterOffset (0x0043E8E0), ConfigureProjectionForViewport (0x0043E7E0), InitializeRaceRenderGlobals (0x0040AE10) [for FPU CW setup]
- **Constants verified via memory_read:** 0x00473BBC..BCC (rdata), 0x004C3710..C3714 (zero-init bss), 0x0045D5F4/0045D6C0/0045D78C (rdata)
- **Tool:** Ghidra MCP, pool TD5_pool14 (read-only)
- **Time budget:** ~80min wall, used ~50min effective
- **Cleanup:** No pool slot held under this script's name — ToolSearch reused pre-existing read-only sessions on pool14. No release needed.
