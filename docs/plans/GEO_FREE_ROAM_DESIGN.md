# F0c: GEO FREE ROAM technical design (F1-F4)

Author: claude-opus-5-5, 2026-10-15. READ-ONLY task, base master @ 29f89b22. Line numbers are
`td5mod/src/td5re/` at that commit unless a path says otherwise.

Evidence tags: **[read]** = I read it in the source. **[survey]** = from a read-only survey agent
(three ran: road pipeline, streaming/render/RT, emitter statics); crux claims were re-read by me and are
tagged [read] where they were. **[est]** = my estimate, not a measurement; F1 must replace it with a logged number.
Sibling inputs: `_archive/roam/F0a/REPORT.md` (casco numbers; the **rotated casco polygon** section is the
authority: diamond of ~5.2 km sides, 27.06 km2, axes at 48.0 / 138.0 deg i.e. -42.13 deg, 484 km drivable,
2,170 ways, 462 casco-aligned 256 m tiles) and `_archive/roam/F0b/REPORT.md` + `PHYSICS_ENTRYPOINTS.md`
(span-consumer audit; its span-field rule is adopted in 2.3; its wedge warning is answered in 2.5).

Units: 430 world units per metre (`td5_geo_route.c:91` [read]); a span / lane is 1500 units = 3.488 m
(`td5_trackgen.h:29` [read]). 24.8 fixed point holds +-8.39 M units.

---

## 0. Key design choices (one screen)

1. **The road surface is stored in the strip's own format.** Every street piece is a run of type-1
   `TD5_StripSpan` quads over `TD5_StripVertex` rows (int16 local offsets from an int32 origin), exactly what
   `probe_span_lane_height` / `triangle_height` (`td5_track.c:7374`, `:7060` [read]) already consume. Junction
   patches are a triangle list in the same vertex format. Consequence: on an identical road the roam height and
   normal are computed by the **same integer code on the same numbers**, so they are bit-identical, not "close".
2. **Ownership unit = one ribbon quad (by its centre-segment midpoint) and one junction cluster (by its
   centroid)**, not "one edge by its midpoint". Rows are a pure function of the edge, so two tiles that share a
   border compute the same border row bit for bit, and the mesh is watertight across the seam.
3. **Junctions are a new builder** (N arms, fillets, fan from the centroid with every arm's own cut-row heights,
   so the patch meets each ribbon exactly). Nothing in the strip code builds an N-arm polygon (see 5.1).
4. **Heights are edge-local**: raster profile, smoothed, grade-capped with the existing three-pass rule, ends
   pinned toward the cluster height. Bridges lift by `layer`, ramps inside the bridge edge and its approaches.
   No global relaxation, so a tile only needs a halo, not the whole city.
5. **Locator = per-tile uniform grid (8 m cells) over quads + triangles + boundary edges**, deck choice by plane
   height under the probe, wall push to the nearest **boundary** edge. Without the lateral-projection hack of
   `geo_resolve_walls` (it would let a car drive head-on through a dead end), and with four anti-wedge rules
   (2.5): one combined push per actor per tick, kerb overhang tolerance, filleted corners, and a minimum swept
   width checked at build time.
6. **Two residency layers.** ROAD layer (~50 KB/tile [est]) resident around every racer and along a race route.
   SCENERY layer (~1 MB/tile [est]) resident around the camera only. Physics never waits for scenery.
7. **World origin set at spawn from F2**, not F9: the casco's north corner (Av 32 x Av 122) is at
   z = 7,019,046 units in the place frame (F0a), 84 % of the 24.8 range, and its east corner at x = 4,255,470
   (5.1 item 2).
8. **Tiles: 256 m on a lattice rotated to the casco axes** (F0a's working choice, kept; 1.8). Rotation is used
   only to decide ownership and to bucket; every stored coordinate stays an integer in the place frame.
9. **Scenery tiles reuse the MODELS.DAT mesh format and `td5_render_span_display_list`**, one display list per
   64 m sub-block, drawn by a new tile pass. No per-tile texture pages (the SRV heap has no free list).

---

## 1. Road world + locator (F1)

### 1.1 What the strip BUILD does today, and what F1 can reuse

| Step | Today | Reuse for roam |
|---|---|---|
| Read ways | `td5_geo_roads.c` one global pool `s_roads` (`:68-76` [survey]), ONE file (`:499`), caps 8192 ways / 65536 pts (`:53-66`). No OSM way id kept, no node ids in the cache at all (`geo_fetch.py:522-531` [survey]) | **Refactor**: lift the pure per-record parsers (`geo_roads_class` `:109`, `geo_roads_lanes` `:438`, `_surface` `:130`, `_sidewalk` `:154`, `_tag_m` `:233`, `_sidewalk_widths` `:262`) into a re-entrant `td5_geo_roads_parse_record(cJSON*, upm, TD5_GeoRoad*)`. Keep the way `id` (needed for edge identity). The roam loader owns its own pool per build context |
| Tile files | `gr_tiled_load` (`td5_geo_route.c:745`) merges whole cache tiles whose INDEX bbox meets a window. A way is stored whole in the tile of its first vertex, INDEX bbox = content extent (`geo_region.py:481-491`, `:600-602` [survey]; F0a: 17,310 ways, 0 duplicate ids) | **Reuse the selection rule**: a roam tile loads every cache tile whose INDEX bbox meets (roam tile + halo). Cache tiles are 859,500 units (1998.8 m) [read PLACE.JSON], so a roam tile normally reads 1-4 cache tiles |
| Junctions | Only in the router: `gr_node_of` snaps to 10 units (`td5_geo_route.c:498` [read]) | **Reuse the rule** (exact 10-unit snap in the place frame), new code: roam node key = `(round_even(x/10), round_even(z/10))` as int64 |
| Resample | `gr_resample` chord resampling at 1500 (`td5_geo_route.c:1397` [survey]) | **Reuse as-is** (export it). Pure |
| Plan-view smoothing | `gr_enforce_curvature` (`:1511`) | **Do not use for roam.** Streets must stay where OSM drew them, or they leave their junctions. Only the route needs the turn limit |
| Lanes / carriageway | Two copies that can disagree for avenidas: `td5_geo_roads.c:590-607` (place floor only) and `td5_geo_route.c:1008-1062` + `gr_mark_divided_carriageways` `:2655` (divided detection, avenida doubling) [survey] | **Refactor into one function** `td5_geo_lane_count(const TD5_GeoRoad*, divided_flag)` used by both. Divided detection (`gr_way_divided` `:2609`) is per way and reusable once its graph input is passed in |
| Pavement width | `td5_geo_sw_resolve(const TD5_GeoSwIn*, int*)` (`td5_geo_sidewalk.c:216`), `td5_geo_sw_carriageway_m`, `_building_line_for`, `td5_geo_roads_pavement_default_m` [survey] | **Reuse as-is** (F4 scenery). Call `td5_geo_sw_place(slug)` once on the main thread before any worker starts (`s_rule` lazy write `:138,146,154`) |
| Height | `td5_geo_height` = bilinear x exaggeration (1.5 default) minus water depth (`td5_geo.c:1190` [read]); `geo_raster_load` slurps the derived window raster | **Refactor**: roam reads the SOURCE `HEIGHT.R16` by window with `gr_raster_read_window` (`td5_geo_route.c:5158` [survey], seek-based, reusable) into a per-build raster, and samples it with the same bilinear + exaggeration + water rule. Same exaggeration as the strip, or F2 stage B differs by design |
| Grade cap | `tg_road_solve` three-pass clamp (`td5_tg_road.c:374`), cap from `s_max_grade` 0.12 x 1.15 headroom, clamp [0.02, 0.20] (`:332`), curvature cap `TD5RE_R22_SMOOTH` (`:363`) [survey]; all over file statics `s_rn[]` | **Copy the algorithm as a pure function** `roam_grade_solve(double *y, const double *t, int n, double cap_per_row, double curv_cap)`. Do not call the span version |
| Road-bed conform | `tg_world_conform_seg` into a global chunk hash, cap 4096 chunks (~50 km2) [survey] | **Not used in roam F1.** Roam ground is the raster; the road is laid ON it. Terrain flattening beside roads is an F4 look question |
| Structures | Route self-crossings (`gr_plan_grade_seps`, lift 3080, ramp 0.5 of cap), water bridges. `layer` is only a yes/no filter; nothing assigns height from it [survey] | **New rule** (1.5). F0a: 37 `bridge=yes`, 45 `layer!=0` (37 at +1, 8 at -1), 2 real tunnels, 8 `building_passage` in the casco box |
| Plaza rings | `pf_build_rings` chains roundabout ways within 60 units (`td5_tg_plazafork.c:204` [survey]) | **Not needed**: in roam a ring is just edges and junctions. The park inside has no surface, so it is walled by construction (Decision "roads only") |
| Divided avenues | Opposite carriageway is a MESH only, "NOTHING HERE IS DRIVABLE" (`td5_tg_avenue.c:14-19` [survey]) | **Not needed**: each carriageway is its own OSM way, so its own drivable ribbon. The median is a gap, walled by construction. Crossings of the median are handled by junction clustering (1.3) |
| Signals | Position only, `GeoSignal {x,z}`, single file, no node id (`td5_geo_signals.c:53-60, 249` [survey]) | Attach to the nearest cluster within 15 m at build time; store cluster id + arm in the tile (F8 consumer) |

Everything in `td5_tg_network.c` (mouth table `s_mouth[span][side]`, `tg_geo_road_hits`), `td5_tg_branch.c`,
`td5_tg_avenue.c`, `td5_tg_plazafork.c` and `td5_geo_attrs.c` is keyed by route span and is **not** reusable for
the road world (survey 5, confirmed in `GEO_MULTI_JUNCTION.md:63-68` [read]: "The mouth table remains one edge
per (span, side)").

### 1.2 Street graph

- **Node** = a 10-unit-snapped point shared by 2+ way vertices (any vertex, not only way ends).
- **Edge** = a maximal run of ONE way between two graph nodes of degree != 2, or between a node and a way end.
  A degree-2 join of two ways (F0a: 4,920 in the primary box) is a **join node**: if both sides have the same
  lane count and width, the ribbons continue through it with a shared row; otherwise a 2-arm junction patch
  (a taper) is built there.
- **Edge identity**: `edge_key = (way_id << 16) | piece_ordinal` (uint64, piece ordinal along the way). Runtime
  edge index is per tile. Direction convention: an edge's row 0 is at the end with the smaller node key, so
  every tile walks it the same way.
- **Attributes**: class, lanes (1.1 lane function), oneway + dir, surface, layer, bridge, tunnel(kind), namek,
  name_id, maxspeed, signal flag per end.
- **Drivable filter**: `roads[]` as written (F0a: no non-drivable leak); decide `bus_stop` (one 0.04 km way) and
  `service` (629 ways, 76.5 km: parking aisles are in there) explicitly. Recommendation: keep `service` but drop
  `service=parking_aisle|drive-through` if the tag is in `tags{}`.

### 1.3 Junction clusters and the patch polygon

Clustering (deterministic, local):
1. Candidate nodes: degree >= 3, and degree-2 joins that need a taper.
2. Union two candidates when they are joined by an edge shorter than `JUNC_MERGE_LEN` (start 30 m) or when
   their discs `r = 0.5 * max arm width + 3 m` overlap. This catches the two carriageways of a divided avenue
   meeting one cross street (two OSM nodes 10-30 m apart), so the crossing is ONE paved patch, not two patches
   with a walled gap between them.
3. Cap a cluster's diameter at 60 m; when a union would exceed it, refuse that union (deterministic because the
   candidate pairs are processed in sorted node-key order).
4. Internal edges (both ends in one cluster) are absorbed into the patch and produce no ribbon.
5. Cluster id = smallest member node key. Owner tile = tile holding the centroid of the member nodes.

Patch polygon for a cluster with arms `a_0..a_{N-1}` sorted by bearing (CCW):
1. Each arm has a centreline direction `d_i` (from the cluster outward, first 15 m of the edge) and half width
   `w_i` (carriageway only, lanes x 1500 / 2).
2. Corner `C_i` between arm i (its left kerb line) and arm i+1 (its right kerb line) = intersection of the two
   offset lines. If the lines are near-parallel (angle < 20 deg) or the intersection is further than 40 m, use
   the bevel point at distance `max(w_i, w_{i+1}) + 2 m` instead.
3. Cut distance `t_i` = max of the projections of `C_{i-1}` and `C_i` on `d_i`, plus a kerb radius
   `r_k` (3 m residential, 6 m primary and above), clamped to [w_i, edge length / 2]. The arm's **cut row** is the
   ribbon row at distance `t_i` from the cluster: its lane-boundary vertices are the patch's arm vertices.
   Row spacing restarts at the cut, so the cut row is row 0 of the ribbon (one function, `roam_arm_cut_row`,
   produces it for both the patch and the ribbon).
4. Between cut rows i and i+1 the outline runs: cut-left of i, fillet arc of radius `r_k` round `C_i`
   (3 segments), cut-right of i+1.
5. Triangulate as a fan from the cluster centroid `P`. Valid when every fan triangle has positive area; if one
   does not (rare star-violating shapes), fall back to ear clipping starting at the lowest vertex index.
6. Heights: `P.y` = cluster height `H_c` (1.4); each arm cut vertex keeps its own ribbon row height; fillet
   vertices interpolate linearly between the two cut rows they join. So every fan triangle is planar, and the
   patch meets every ribbon exactly on the shared vertices. A flat patch would put a kink at every sloped arm.
7. Integer safety: `triangle_height` forms int32 products of edge components (`td5_track.c:7086-7088` [read]).
   With every triangle edge <= 20,000 units (46.5 m) the products stay <= 4e8 and their differences
   <= 8e8 < 2^31. Split any longer fan triangle by inserting a midpoint on its outer edge.
8. Boundary edges: every patch outline segment, plus the outer kerb edge of every ribbon quad. Edges between two
   drivable primitives are interior and never produce a wall. Closed-off mouths at race time are NOT tile data
   (barriers are race objects, Decision table).

Divided-avenue median gaps that OSM maps as a short connector way between the two carriageways are absorbed by
rule 2. A median break with no OSM connector stays a wall, which matches OSM.

### 1.4 Heights (edge-local, order-independent)

All in the place frame, double precision, computed under the main thread's FP environment (1.6).

- `H_ground(x,z)`: bilinear on the source HEIGHT.R16 window x exaggeration, water rule as `td5_geo_height`.
- Cluster height `H_c` = mean of `H_ground` at the member nodes and at the patch outline vertices.
- Edge profile, rows `k = 0..n` from the row-0 end:
  1. target `t_k = H_ground(centre_k)` smoothed with a triangular window of +-5 rows (35 m);
  2. blend toward `H_c(a)` over the first 8 rows and toward `H_c(b)` over the last 8 (cosine weights);
  3. add the structure lift `L_k` (1.5);
  4. `roam_grade_solve` (forward, backward, forward clamp, cap 0.12 x 1.15, the strip's numbers) with rows 0
     and n pinned to the blended ends.
- Lateral: flat across the carriageway in F1 (no camber). Every lane-boundary vertex of row k gets `y_k`.
- Quantisation: compute the place-frame integer once, `Xi = (int32)floor(x + 0.5)` style rounding written
  explicitly (not relying on the CRT rounding mode), then store tile-local = `Xi - tile_origin`. Two tiles
  produce the same world integer for a shared row because the integer is formed BEFORE the tile offset.

Why edge-local is enough: the only inputs to an edge's rows are its own polyline, its two cluster heights and
the raster under it. A cluster's height needs only its member nodes and its arms' first 40 m. So a tile needs a
**halo** of (longest edge it owns a quad of) for the profile. Long edges (rural trunks) make the halo large; cap
it by splitting any edge longer than 1 km at a synthetic join node every 1 km (deterministic positions along the
edge from row 0), so the halo is <= 1 km + 60 m.

### 1.5 Grade separations

- `bridge=yes` edge with `layer >= 1` crossing (XZ intersection) an edge of lower layer: the deck needs
  `y_deck(c) >= y_under(c) + 3080` (`GR_XSEP_LIFT_UNITS` 3080 = 7.16 m, the strip's figure [read
  `td5_geo_route.c:99`]). `L_k` = the required lift at each crossing row, spread along the bridge edge and up to
  2 approach edges either side with the 0.5-of-cap ramp rule (`GR_XSEP_RAMP_FRAC` [read `:105`]), and 0 at any
  row that is a cluster cut row of an at-grade cluster. If the ramp does not fit, log `ROAM_RAMP_SHORT` with the
  edge key and accept up to 0.20 grade.
- `tunnel=yes` (2 trunk ways in the casco box): negative lift of the same rule (bore).
- `tunnel=building_passage` (8), `layer=-1` without `tunnel`: at grade.
- Clusters never merge across layers: a node whose ways have different layers is not a junction (OSM does
  not join them), so the snap rule already keeps them apart. Assert it.
- The halo for a bridge is its approaches (<= 300 m) [est], inside the 1 km halo.

### 1.6 In-memory structs

```c
/* td5_roam_world.h */
typedef struct {            /* one resident tile, ROAD layer */
    int32_t  tx, tz;                    /* tile index in the place frame            */
    int32_t  org_x, org_z;              /* tile origin, PLACE frame units           */
    int32_t  wx, wz;                    /* origin in the current WORLD frame        */
    TD5_StripSpan   *span;   int nspan; /* type-1 ribbon quads, origins tile-local  */
    TD5_StripVertex *vert;   int nvert; /* <= 65535 (uint16 indices)                */
    RoamTri  *tri;           int ntri;  /* junction patch triangles                 */
    RoamBEdge *bedge;        int nbedge;/* wall edges                                */
    RoamEdge *edge;          int nedge; /* owned edge pieces: key, attrs, row range  */
    RoamNode *node;          int nnode; /* owned clusters                            */
    RoamGrid  grid;                     /* CSR, 32x32 cells of 3440 + 1-cell margin  */
    uint8_t  *arena;                    /* ONE allocation; free = free(arena)        */
    uint32_t  stamp;                    /* input hash, see ROAMTILE header           */
    int       state;                    /* BUILDING / READY / RETIRED (+frame)       */
} RoamTile;

typedef struct { uint16_t v[3]; uint16_t patch; uint8_t surface, flags; int32_t ox, oy, oz; } RoamTri;
typedef struct { uint16_t v[2]; uint16_t prim; uint8_t kind, layer; } RoamBEdge;   /* kind: kerb, deck */
```

Ribbon spans use the strip record unchanged: `span_type = 1`, lane count in the low nibble of byte +3,
`left/right_vertex_index` into `vert`, `link_next/prev` = next/prev quad of the same ribbon in this tile or -1,
`origin_*` = tile-local int32 (converted to world at install by adding `wx/wz`; a later origin shift rewrites only
these origins). `surface_attribute` from the way's surface.

### 1.7 ROAMTILE binary format (`re/assets/geo/<place>/roam/ROAMTILE_<tx>_<tz>.BIN`)

Little-endian, sections 16-byte aligned, offsets from file start.

```
header (64 B)
  char     magic[8]      "TD5ROAM1"
  uint32   version       ROAM_FORMAT_VERSION
  uint32   rules         ROAM_RULES_VERSION (bumped by hand when a build rule changes)
  uint32   input_hash    FNV-1a over: place slug, PLACE.JSON tiled origin + tile size, tile size,
                         every rule knob value, the ROAMGRID.JSON bytes (1.8), the INDEX rows of
                         the cache tiles read,
                         the HEIGHT.R16 header, and the byte size + FNV of each cache tile file read
  int32    tx, tz
  int32    org_x, org_z  place-frame origin of this tile
  uint32   nsect
  uint32   reserved[5]
section table: nsect x { uint32 tag, offset, bytes, count }
  'SPAN'  TD5_StripSpan[nspan]          (24 B, the STRIP.DAT record)
  'VERT'  TD5_StripVertex[nvert]        (6 B)
  'TRI '  RoamTri[ntri]
  'BEDG'  RoamBEdge[nbedge]
  'EDGE'  RoamEdge[nedge]   { uint64 key; uint32 node_a, node_b; uint8 klass, lanes, oneway, surface;
                              int8 layer; uint8 flags; uint16 name_id; int32 length;
                              uint16 span_first, span_count; uint16 row0; }
  'NODE'  RoamNode[nnode]   { uint64 cluster_key; int32 x, y, z; uint8 narm, flags;
                              uint16 tri_first, tri_count; uint16 arm_first; }
  'ARM '  RoamArm[]         { uint64 edge_key; int16 bearing12; uint16 cut_span; }
  'GRID'  uint32 nx, nz, cell, x0, z0; uint32 start[nx*nz+1]; uint16 item[]   (item high bit: tri vs span)
  'SIGN'  { uint64 cluster_key; uint8 arm, kind; }
  'NAME'  interned UTF-8 names used by EDGE.name_id
  'STMP'  text: the build identity (like GENSTAMP.TXT), for humans
trailer: uint32 fnv1a of every byte before it
```

The cache is an optimisation only: a missing or stale file (input_hash mismatch) means "build it". F1's
acceptance test (same bytes twice, in two build orders) runs on the built bytes, with the cache disabled.

### 1.8 Tile size and lattice: 256 m, rotated to the casco axes (F0a's choice, kept)

F0a's casco-aligned 256 m lattice gives 462 tiles over the 27 km2 diamond, median 1.1 km of road, 6 way pieces,
6 junction nodes, 205 buildings (1,190 vertices), max 407 buildings, 1,934 border-crossing segments. I found no
reason against it, provided rotation never touches stored geometry:

- **Lattice definition** = one small file per place, `re/assets/geo/<place>/roam/ROAMGRID.JSON`: origin
  `(ox, oz)` (place-frame integers), angle as the exact double pair `(c, s)` written with 17 significant digits,
  tile size `110080` units (256 m x 430). Its bytes go into every tile's `input_hash`, so changing the lattice
  invalidates every cached tile.
- **Tile of a point**: `u = c*(x-ox) + s*(z-oz)`, `v = -s*(x-ox) + c*(z-oz)`, `tx = floor(u/110080)`,
  `tz = floor(v/110080)`, in double, under the main thread's FP env (1.10). Used ONLY for two things: deciding
  which tile owns a quad / cluster / building at build time (the answer is then stored, never recomputed), and
  picking candidate tiles in the locator.
- **Locator candidates**: the tile of p plus every neighbour whose rotated square, grown by a 1-cell margin,
  contains p. The margin absorbs any rounding difference, and the candidate tests themselves (point in quad /
  triangle, plane height) run on integer place-frame coordinates, so the rotation cannot change a result.
- **Stored coordinates**: tile origin = the lattice corner rounded to integer place units; spans, vertices and
  triangles are integers relative to it, exactly as in 1.4. A shared border row is the same place-frame integer in
  both tiles because it is formed before the origin is subtracted.
- **Locator grid**: 32 x 32 cells of 3440 units (8 m) in the tile's rotated frame, CSR as in 1.7.
- **Raster window**: the axis-aligned bounding box of the rotated tile plus the halo (1.41x the area of an
  aligned tile: ~73 x 73 raster cells grow to ~104 x 104 before the halo). Harmless.
- **Lattice phase (optional, F1 measures)**: La Plata's streets run along these axes, so a tile border can lie
  on top of a street centreline, which makes consecutive quads of that street alternate between two tiles by
  midpoint rounding. Ownership stays exact, but render and locator do more cross-tile work. If F1's census shows
  borders hugging streets, shift `(ox, oz)` by half a block so borders fall mid-block.
- **Beyond the casco (F9)**: the lattice is unbounded, so the partido uses the same rotated tiles. Streets out
  there do not follow the casco axes, which costs nothing.

### 1.9 Locator API

```c
typedef struct {
    uint32_t prim;        /* (resident slot << 20) | (is_tri << 19) | index; ROAM_PRIM_NONE */
    uint64_t edge_key;    /* street piece or cluster key                                    */
    int32_t  ground_y;    /* 24.8, exactly triangle_height()'s return                        */
    int16_t  normal[3];   /* int16 unit normal, exactly triangle_height()'s out_normal       */
    uint8_t  surface;     /* TD5_SURFACE_*                                                  */
    uint8_t  inside;      /* on the drivable surface of the chosen deck                     */
    uint8_t  capped;      /* the upward extrapolation cap fired (same rule as the strip)    */
    uint8_t  is_patch;
    int8_t   lane;        /* ribbon lane, -1 on a patch                                     */
    int8_t   layer;
    int32_t  along;       /* units from row 0 of the edge                                   */
    int32_t  lateral;     /* signed offset from the edge centreline                         */
    int32_t  push_x, push_z, push_dist;   /* to the nearest boundary edge when !inside      */
} TD5_RoamHit;

/* x, z, y in 24.8 WORLD frame (actor units). has_y = 0 for XZ-only callers (camera, shadow).
 * hint = previous hit's prim for continuity (ROAM_PRIM_NONE ok). Returns 0 only when no
 * resident tile covers the point. */
int td5_roam_locate(int32_t x, int32_t z, int32_t y, int has_y, uint32_t hint, TD5_RoamHit *out);
```

Algorithm:
1. World to place: `px = FP_TRUNC(x) + world_origin_x` (int64 intermediate).
2. Tiles: the containing tile plus each neighbour whose grid margin contains p (at most 4).
3. Candidates: the items of p's cell. Quads: AABB then the convex test (`geo_point_in_quad` logic,
   `td5_track.c:1786` [read]). Triangles: 3 edge signs.
4. Height per candidate with the strip's own code: quads pick the triangle by `diagonal_cross(vl0, vr1, ...)`
   exactly as `probe_span_lane_height` does for type 1 (`td5_track.c:7404-7458` [read]: for type 1 all three
   branches use that diagonal), then `triangle_height`.
5. Deck: among candidates, the highest `h <= y + 430*256` (1 m step up); if none, the lowest `h` above. Ties:
   same edge as the hint, then lower prim. When `has_y = 0`: the hint's edge, else the highest.
6. Deck walls: if the hint is a deck primitive (layer >= 1) and the only containing primitives are more than
   1.5 m below the probe, report `inside = 0` and push to the deck's boundary edge (the bridge side is a wall;
   a car does not drop through the side of a bridge onto the road below).
7. Outside: nearest boundary edge among BEDG items in cell rings 1..3 (21 m), restricted to the hint's layer
   class; beyond that, a scan of every resident tile's BEDG (the `geo_nearest_global` fallback). `ground_y` =
   that edge's primitive plane extrapolated, with the same upward cap as `triangle_height`
   (`vmax + (vmax-vmin) + 1024`, `td5_track.c:7140-7152` [read]), so an off-road wheel behaves as on the strip.

Refactor needed for step 4 (one file, byte-identical for the strip): `triangle_height`, `diagonal_cross` and
`get_quad_vertices` read the global `s_vertex_table` through `vertex_at`. Give them a `const TD5_StripVertex *vt`
parameter (the strip passes `s_vertex_table`), export as `td5_track_tri_height_v` / `td5_track_quad_height_v`.

### 1.10 Determinism rules for the builder

- Apply the main thread's FP env on any worker first (`td5_plat_fpenv_apply`, as `td5_trackgen_stream.c:88`
  [read]); without it the stream worker's MODELS.DAT differed by 492 bytes (`:40-46` [read]).
- Iterate sorted arrays only (ways by id, nodes by key, edges by key). No hash-table iteration order, no
  pointer order. Draw no game RNG.
- Never read the player position or load order inside a build.

### 1.11 F1 dev viewer

Free-cam already exists (`verify/geo_tour.ps1`, picker `td5_pick.c`). F1 draws the road layer as flat-shaded
debug triangles through the F12 collision overlay path (white = boundary edges, cyan = interior, yellow = the
locator's current prim), no scenery. This is enough to see coverage, seams and junction shapes.

---

## 2. Physics fidelity

### 2.1 What the contact path consumes (F0b, confirmed)

Per racer per tick: 4 wheel heights + normals + capped flag + surface, 4 body-corner heights (feed
`td5_physics_compute_surface_gravity`), the chassis `heading_normal`, and wall pushes at the 4 body corners.
Producers: `td5_track_update_probe_position` (walker), `td5_track_compute_contact_height_bounded`
(`td5_track.c:7606` [read]), `td5_track_get_surface_type` (`:5376` [read]),
`td5_track_compute_runtime_heading_normal` (`:8105` [read]), `td5_track_resolve_wall_contacts` (`:2502` [read]).

The height is `triangle_height` of one lane quad's triangle: int32 cross product `>> 12`, normalised to an
int16 unit normal with the `uny == 0 -> 1` guard, plane solve in integer units, upward cap, result `<< 8`
(`td5_track.c:7060-7166` [read]). Wheel probes walk one span per call (`single_step = 1`, `:6927` [read]).

### 2.2 What the locator must return for byte-close behaviour

The same three vertices and the same origin the strip would pick for that wheel. With 0.1 (strip format) and
1.9 step 4 (same triangle choice, same function) that is automatic on any ribbon. Remaining differences are
(a) the **walker**: the strip wheel walker can lag one span behind a wheel that crossed two span boundaries in a
tick (`:6920-6926` [read]) while the locator always finds the containing quad, and (b) the **chassis span hint**
used by `_bounded` (+-1 lane around the carried sub-lane, `:7606-7640` [read]); the locator picks the containing
lane. Both only matter at a span boundary at high speed and are visible in the A/B below as single-tick spikes.
If they matter, the provider can emulate them (keep a per-probe "last span" and advance at most one row per
call), which restores bit identity at the price of the strip's known lag.

### 2.3 Hookup

`td5_roam_active()` (one global). Actor span state follows **F0b's rule** (F0b REPORT "Actor span fields in
roam"): the strip is unloaded (`s_span_count == 0`), `g_td5.track_span_ring_length = 0`, all four span fields and
the lane are -1 (written after `td5_ai_invalidate_actor_track_state`, which writes 0, `td5_ai.c:1366`), and the
roam edge / prim lives in a side table keyed by slot: `s_roam_probe[slot][8]` (`TD5_RoamHit`, 4 wheels + 4 body
corners) plus `s_roam_chassis[slot]`. `TD5_TrackProbe` and the 0x388 actor layout are untouched.

**F2 must-fix from F0b:** `td5_compute_heading_delta` indexes `route_table[(uint16)span*3+1]` with no check
(`td5_camera.c:5727`, F0b verified); with span = -1 that is index 196,606. Guard it (or gate the caller off in
roam) in the same round that turns roam on.

Seam: F0b lists 8 `td5_track.c` functions to replace (`update_actor_position` 6745, `update_probe_position`
6910, `compute_contact_height_bounded` 7606, `_with_normal` 7482, `get_surface_type` 5376,
`compute_runtime_heading_normal` 8105, `resolve_wall_contacts` 2502, `last_contact_was_capped` 7059) plus the
span-0 fallback at `td5_physics_suspension.c:3781`. All 8 get a roam early-out (that covers camera, shadow, HUD
and AI callers, XZ-only). The physics call sites additionally call the provider directly, because the existing
signatures carry neither the actor nor a Y (`update_probe_position(probe, x, z)`, `_bounded(span, lane, x, z,
normal)`), and without the probe's Y a wheel under a bridge cannot tell the deck from the road below:

| Site | Roam branch |
|---|---|
| `td5_physics_refresh_wheel_contacts` wheel loop, `td5_physics_suspension.c:3723-3839` [read] | `td5_roam_wheel_contact(actor, i, x, z, y, &ground_y, normal, &surface, &capped)` replaces walker + contact vertices + `_bounded` + surface + capped |
| body-corner loop `:4072-4110` [survey F0b] | `td5_roam_corner_height(actor, i, ...)` |
| chassis walker + heading normal `:2079-2101`, `:2396-2435`, `:3245-3261` | `td5_roam_actor_locate(actor)` writes `heading_normal` (never `uny == 0`) and sets `track_span_raw` per F0b's rule |
| `td5_physics.c:1710-1712` wall resolvers | `td5_roam_resolve_walls(actor)` |
| recovery mode `td5_physics.c:5728-5780` | same provider calls |
| camera / shadow / HUD callers of `td5_track_probe_height` etc. | early-out inside those `td5_track_*` functions to `td5_roam_locate(..., has_y = 0, ...)` |

Spawn: `world_pos.y = 0xC0000000` ground-snap sentinel (F0b) works only if the provider is live on the first
integrate tick; set Y from `td5_roam_locate` at spawn instead.

### 2.4 Walls: one change from `geo_resolve_walls`

F0b proposes reusing `geo_resolve_walls` (`td5_track.c:2314` [read]) verbatim. Reuse it **except the lateral
projection** at `:2331-2347` [read]: it drops the push component along the car's heading because, on the strip
quad soup, the nearest edge was sometimes a span's internal transverse edge at a fork crack. On a watertight roam
mesh no internal edge is a boundary edge, so the projection only removes real walls: a car driving straight at a
dead end, the far kerb of a T junction, or a closed bridge end would have the push cancelled and drive through.
Note also that `geo_resolve_walls` is the primary wall handler only on REVERSE native tracks today
(`geo_active`, `:1761` [read]); on forward tracks it was tried and "WEDGES a car at a sharp fork bend"
(`:1754-1760` [read]). Roam is its first forward use at scale, so F2 must test hairpins (a plaza ring) and narrow
streets, not only straights.

### 2.5 Why the roam wall does not wedge (answer to F0b's flag on `td5_track.c:1749-1760`)

What the comment records: `geo_resolve_walls` used as the PRIMARY forward wall handler "WEDGES a car at a sharp
fork bend (the per-tick nearest-edge push fights the turn), which stops the race finishing" [read]. It does not
record the mechanism. Reading the code gives three contributors, each with a roam answer:

1. **Up to four independent full corrections per tick that can oppose each other.** `geo_resolve_walls` calls
   `td5_physics_wall_response` + `td5_physics_rebuild_pose` once per offending BODY corner (`td5_track.c:2319-2357`
   [read]). Each call translates the whole chassis by that corner's full penetration along the wall normal
   (`td5_physics_collision.c:222-228` [read]: `push = penetration - 4`, applied to `world_pos`) and applies a
   normal impulse with a yaw lever arm. When the corridor is narrower than the body's swept width at that
   curvature, the front-outer corner is outside one wall and the rear-inner corner outside the other: the two
   translations cancel, the impulses scrub speed every tick, and the car stays in the same place. The autotrack
   fork carriageways are half-width (`geo_active` note "half-width fork", `:1755` [read]; I did not measure the
   width), so a sharp fork bend is the likely case. This is the most plausible mechanism, not a reproduced one.
   **Roam:** one combined resolution per actor per tick. Collect the outside corners, take the one with the
   largest penetration as THE contact and apply one `wall_response` for it. If two outside corners have push
   normals with a negative dot product (a pinch), apply no translation that tick, only the impulse of the deeper
   one, and log `ROAM_WALL_PINCH` with the cluster/edge. A pinch is a builder bug in roam (rule 4), not a driving
   situation.
2. **Push direction that jumps between ticks.** At a sharp corner the nearest boundary point flips between the
   two kerb lines meeting at a vertex, so the push normal swings by the corner angle from one tick to the next.
   With the lateral projection (`:2331-2347`) on top, a push that is mostly along the heading is cut to a small
   lateral remainder that points into the turn on one tick and out of it on the next.
   **Roam:** every block corner is a fillet arc of radius `r_k` >= 3 m (1.3 step 4), so the boundary normal near
   a corner is continuous and the push turns smoothly round it. Reflex vertices that remain (bevels, ear-clip
   fallbacks) are listed in the F1 census. And no lateral projection (2.4).
3. **The probes are body corners, which legitimately overhang a kerb.** Any overhang beyond the drivable edge
   counts as penetration today (`if (h.inside || h.pushdist <= 0) continue;`, `:2327` [read]).
   **Roam:** a corner is only a wall contact when it is more than `ROAM_KERB_OVERHANG` (0.35 m = 150 units)
   outside, the body overhang a real car has over a kerb. Wheels never get this tolerance: a wheel outside the
   surface reads the extrapolated plane (1.9 step 7), as on the strip.
4. **Build-time guarantee: every corridor fits a car.** For every ribbon and every pair of adjacent arms of a
   cluster, F1 checks the swept width of a reference body (TD5 car bounding box, ~4.6 x 1.9 m [est]; read the
   real figure from the car table) on the tightest turn the geometry allows:
   `need = sqrt((R + W/2)^2 + Lf^2) - (R - W/2)` with R the inner-kerb radius. A one-lane service road (3.49 m)
   turning on a 5 m centreline radius needs about 2.9 m [est], so it fits; a corner that does not fit gets its
   fillet radius increased up to the arm cut limit, then is logged `ROAM_SWEPT_FAIL`. Carriageway widths in the
   casco come from the La Plata rule (10 m calle/avenida carriageways, `td5_geo_sidewalk.c:71-101` [survey]), so
   failures should be rare and countable.

F2 test for this (added to the F2 acceptance): a scripted drive of 90-degree turns at 20 clusters of each
kind (one-lane service, calle x calle, avenue x diagonal, a plaza ring lap) at 40 / 80 km/h with the AI steering
model, counting per turn: `ROAM_WALL_PINCH` (must be 0), ticks with speed lost to walls while the car is inside
the corner's fillet zone (report), and stuck events (speed < 1 km/h for > 2 s with throttle on, must be 0).

### 2.6 F2 A/B test (two stages)

**Stage A, provider identity (must be bit-exact).** A dev knob `TD5RE_ROAM_MIRROR=1` builds a one-tile roam
road layer that is a 1:1 copy of the loaded strip's type-1 spans (same vertex rows, same origins, world frame
= strip frame) and routes the racer contact calls through the roam provider while everything else (AI, camera,
render) keeps using the strip. Run the same scripted input twice (`--InputScript`, fixed seed, `[Trace]
RaceTrace=1 Modules=pose,motion,rotation,calls`, module names from `td5_trace.c:191-202` [read]; `calls` carries the
`triangle_bary` return that `triangle_height` emits, `td5_track.c:7161` [read]), mirror off vs on, on level 90 seed 2054677019 (synthetic) and level091.
Pass: `pose` and `motion` CSVs byte-identical for every tick in which all four wheels are on type-1 spans
(ticks touching other span types are excluded and counted). Any diff is a provider bug, not a builder question.

**Stage B, builder fidelity (tolerance).** Pick one straight OSM way of constant lanes, >= 500 m, no junction
inside, that the level091 route drives (choose it in F1 from ROUTE.JSON). Drive the same scripted line
(constant throttle, steering zero after alignment) in the strip BUILD and in roam (spawn mapped by lat/lon).
Compare per tick after removing a constant Y datum: wheel ground height (|d| <= 0.05 m), display pitch and roll
(|d| <= 1 deg), speed (|d| <= 1 km/h), wall contacts (0 on both). Differences here are attributable to the builder
(the strip lowers terrain-relative heights to `y0 = 0`, `td5_tg_road.c:2112-2117` [survey], and uses
`tg_world_h` with a synthetic detail octave, `td5_tg_world.c:141-143` [survey]), so Stage B must report them
with the cause, not tune physics to hide them.

---

## 3. Streaming (F3)

### 3.1 What the template gives (from `td5_trackgen_stream.c`, survey + [read])

Raw `CreateThread` worker, FP env captured on main and applied first in the thread (`:88`, `:233-234`),
one-way `volatile int` cancel polled between units, `InterlockedExchange` done flags, join in the per-frame tick
(`td5_tgstream_tick`, called from `td5_game_tick` at `td5_game.c:1498` [read]), synchronous fallback when
`CreateThread` fails, countdown hold gate (`td5_tgstream_hold`, 30 s cap). Publication: release fence before the
`.count` gate, acquire load on read (`td5_track.c:727`, `:916-918` [read]). Nothing is ever freed while racing
(republish appends, `:937-943` [read]). The worker creates no GPU resource: raster geometry is CPU-transformed
and streamed through a 32 MB per-frame upload ring (`d3d12_backend.c:165, 3812` [survey]); RT BLASes are built
on main in `td5_rt_frame`.

What does NOT carry over: one level-global blob and display-list table freed only all at once
(`free_models_dat_runtime`, `td5_track.c:369`), a global mesh hash whose rehash frees, level-wide destructive
passes (`derive_missing_normals` frees all, `register_lamp_lights` resets), RT keyed by entry index with a full
destroy when the count changes (`td5_rt.c:864`), and `td5_jobs` is fork/join only and not re-entrant
(`td5_jobs.h:9-15`) [all survey].

### 3.2 Worker design

- `td5_roam_stream.c`: ONE persistent worker thread (same lifecycle code as the template). A second worker is
  possible for the ROAD layer only once the road builder is re-entrant (it is designed to be: 1.6 per-tile arena,
  per-build pools), but the F4 scenery emitters will start single-instance (section 4), so F3 ships one worker.
- Request queue: fixed ring of `{tx, tz, layer, priority, gen}` written by main only (SPSC). Priority = distance
  from the nearest racer (road) or camera (scenery), recomputed each frame; the worker takes the best pending
  request. `gen` lets main cancel a request (worker checks `gen` between build phases and drops stale work).
- Build target: a private `RoamTile` in its own arena. Road tile: ROAMTILE cache hit = read + verify + fix up
  pointers; miss = build + write the file (temp name, then rename; note `td5_plat_file_rename` returns 0 on
  success). Scenery tile: F4 emitters into a private MODELS-format blob owned by the tile.
- Done queue: SPSC ring, release fence on push; main pops with acquire.

### 3.3 Publish fence and install (main thread, between frames)

Called from the top of `td5_game_tick` next to `td5_tgstream_tick`, i.e. NOT while panes record (panes record
on job workers under `TD5_MT_PARALLEL_BUILD`, `td5_game.c:8277-8286` [survey]):
1. Pop at most ONE scenery tile and up to 4 road tiles per frame (hitch budget; measure install time and log it).
2. Road tile: convert span origins to world (`+ wx/wz`), insert into the resident hash (fixed 64-slot table,
   open addressing by (tx,tz)); the locator sees it from the next sim tick.
3. Scenery tile: per-tile versions of the deferred passes (dim additive billboards, derive missing normals for
   THIS tile's meshes, banner page scan, lamp registration with an owner id so it can be removed), then link its
   display lists into the draw table, then queue its RT feed.

### 3.4 Resident set

| Layer | Who needs it | Set | Est. size |
|---|---|---|---|
| ROAD | physics of every racer, AI, minimap later | 3x3 tiles round each racer + the next ring in its velocity direction; during a race, every tile the route polyline touches +-1, pinned | ~50 KB/tile [est] |
| SCENERY | renderer, RT | 5x5 round the camera (+-640 m) + the next ring ahead when speed > 20 m/s | ~1 MB/tile CPU [est], +RT VB/IB when RT is on |

5x5 because the fixed far cull is 195,000 units = 453 m (`DEFAULT_FAR_CULL`, `td5_render_mesh.c:3920`
[survey]); with the camera anywhere in the centre tile, 3x3 guarantees only 256 m to the set's edge, 5x5
guarantees 512 m. AI racers far from the camera keep only ROAD tiles, so
they drive on real geometry with no scenery cost. A racer about to reach a non-resident ROAD tile is a bug:
the margin is one ring (256 m, 3 s at 300 km/h) and a road tile build must take << 100 ms [est; F1 logs it]. If it
happens anyway, build that tile synchronously on main and log `ROAM_ROAD_STALL` (a hitch, never a fall-through).
Unload: Chebyshev distance > set radius + 1 for 2 s.

### 3.5 Unload

Main thread: unlink from the resident hash and the draw table, remove its lamps, `Backend_RTMeshDestroy` its RT
meshes (already deferred-safe: `dxr_free_mesh` retires through `d3d12_priv_retire`, survey), mark RETIRED with the
frame number, and `free(arena)` after `D3D12_FRAME_COUNT` (2) + 1 frames, so no recorded command list or job-worker
pane can still read its CPU vertices.

### 3.6 Render integration

- New `td5_roam_render_tiles(view)` called inside `td5_render_actors_for_view` right after the span entry loop
  (`td5_render_mesh.c:2863-2903` [survey]); in roam the strip has no entries, so that loop draws nothing and the
  fallback ribbon is gated off.
- Each scenery tile carries 16 display lists (4x4 sub-blocks of 64 m), each a stable `TD5_SpanDisplayList`
  (pointer identity is used for dedup, `td5_types.h:898-906` [survey]). The pass sorts resident tiles near to far,
  culls sub-blocks with `td5_render_is_sphere_visible` (`td5_render_mesh.c:1263`) and calls
  `td5_render_span_display_list`, which keeps per-mesh sphere cull, billboards, lighting and page binding. Heap
  mesh pointers pass the renderer's checks (address >= 0x100000 plus `td5_track_is_valid_mesh_ptr`,
  `td5_render_mesh.c:1582-1617` [read]).
- Mesh frame: tile-local vertices with `mesh->origin_*` = tile origin in the world frame (rewritten on an origin
  shift together with `bounding_center_*`). Do NOT use `s_dl_z_offset` (Z only, not per pane [survey]).
- Road surface rendering: the ROAD layer's own triangles textured with the road page (F2 "plain road/ground
  render"), emitted as a per-tile mesh in the same format, so F2 and F4 share one draw path.
- Ground: in F2 a per-tile heightfield mesh from HEIGHT.R16 (cell 1500, 72x72 quads) drawn under the road,
  pushed down 5 cm so the road never z-fights.

### 3.7 RT layer

`td5_rt_level_build` walks all strip spans once (`td5_rt.c:325` [survey]); scenery is fed by an entry window with
hysteresis and a 2-per-frame budget (`rt_scene_update` `:854`). Roam needs: a per-tile road BLAS (from the ROAD
layer) and per-sub-block scenery BLASes, keyed by (tile slot, sub-block) instead of entry index, through the same
feed/evict/budget logic. Pools are VB 128 MB / IB 48 MB, `DXR_MAX_MESHES 4096` [survey]: 30 tiles x 17 meshes =
510 meshes fits. RT feed vertices are world space (`td5_rt.c:584` [survey]), so an origin shift destroys and
re-feeds every resident tile's BLASes (rare, F9). **Recommendation: F3 runs roam with RT forced off; the RT tile
feed is its own sub-task (F3c) after streaming is stable.** Tests already run RT off (memory rule).

### 3.8 Memory budget

[est] numbers, F3 must log real ones per tile (`ROAM_TILE bytes=...`):
- ROAD layer p95 tile (1.68 km of way, F0a): 1,680 m / 3.488 m = ~480 rows; at ~2.3 lanes per way [est]
  ~1,100 lane quads (24 B each, 26 KB), ~480 x 3.3 = ~1,600 verts (6 B, 10 KB), 15 clusters x ~14 tris
  = ~200 patch tris (4 KB), grid ~10 KB, edges/nodes ~3 KB: ~55 KB (p95 from F0a's north-up 256 m table; the
  casco-aligned median is 1.1 km). Whole casco (462 tiles, F0a) loaded at once for F2: ~25 MB.
- SCENERY tile: F0a casco-aligned median 205 footprints / 1,190 vertices, max 407. At the max: ~2,300 raw vertices
  -> ~1,900 wall quads + roofs ~5.6k tris, at 44 B per `TD5_MeshVertex` and unshared quad corners ~10k verts =
  440 KB, commands ~90 KB; pavements, kerbs, furniture, trees at least as much: ~1-1.2 MB. 30 resident:
  ~35 MB CPU.
- Texture pages: none per tile. Roam loads one shared page set at roam start (road, pavement, facade, roof,
  foliage pages already generated by `td5_tg_pages.c`) and tiles reference ids only. Reason: the SRV staging
  heap has no free list and wraps at 8192 (`d3d12_backend.c:2033-2034` [survey]), and the level page zone is 800.

---

## 4. Scenery port (F4)

### 4.1 Inventory (survey, 18 modules, live source; codemap undercounts)

The plan's "about 102 mutable statics" comes from a comment in `td5_trackgen_stream.h:26` [read]. The real
figure is **422 mutable file-scope declarations** [survey], plus function-local static scratch arrays in 7
modules. `codemap/statics.tsv` holds 338 rows for these modules [read, grep count], but it lists `const`
tables, records only the first name on multi-name lines and misses anonymous `static struct {...} s_x;`
blocks (`s_gb`, `s_fw`, `s_gs`, `s_gdrop`) and non-static globals (`s_forks`, `s_r12_flora`, `s_turn_side`,
`s_guard_ex_*`) [survey].

| Class | Count | What it means for a per-tile build |
|---|---:|---|
| (a) input caches from disk (`s_gb` buildings, `s_fw` footways, `td5_tg_geo_clear.c` road-segment grid, landmark tables) | 18 | Already world-space. Share READ-ONLY across tile builds. Load on main before any worker starts |
| (b) per-build working state (prepass plans keyed by span: signals `s_sigp`, crossings `s_gx`, bus stops `s_bs`, footpaths `s_fp`, prefabs `s_pf`, network `s_mouth`, forks, parks; ambient pointers `s_geop_surf`, `s_xst_apron`, `s_net_apron`, `s_farcap`; order-dependent `s_r15_store_last`) | 66 | Move into the tile context; re-key the plans to (edge, side, along) |
| (c) output + census counters (`s_scn`, `s_meshtag`, `s_acct_*`, mast-arm atlas, ~200 `long` census counters) | 270 | Counters -> one `TG_SceneryStats` per context, summed into the global census on main at install. Output -> the context's buffers |
| (d) knob caches | 25 | Read once on main before workers start (benign) |
| (e) memo tables (crossing memos, pavement widths `s_geo_sw_w[span][2]`, turn map, water table, biome grid, `s_r12_flora`) | 43 | Re-key to (edge, side) or world hash; `s_r12_flora` crosses entries and needs a world spatial hash with a tile halo |

26 tables are sized `[TD5_TG_MAX_SPANS]` in scope (+11 more read through span predicates in
`td5_tg_road.c`/`td5_tg_bridge.c`) [survey]. The span frame is reached through `tg_road_edge` (34 call sites),
`tg_city_edge_frame` (38), `tg_carriageway_reach*` (68), `tg_road_half_width` (35), raw `nl->v[...]` (453) and
span predicates (374) [survey, grep counts]. That count is the real size of F4.

What is cheap: every write primitive is a pure function of `(TG_Buf*, world xyz)`: `tg_write_quad_mesh`
(`td5_tg_city.c:535`), `_col` (`:593`), `tg_write_prefab_mesh` (`:465`), `tg_emit_box_mesh` (`:318`),
`tg_emit_billboard_mesh` (`:377`), producing MODELS.DAT mesh records (0x38 header, 16 B commands, 44 B vertices)
[survey]. The emitter contract `TG_FBHook {nl, si, nspans, lanes, b, blk, moff, nmesh, maxmesh}`
(`td5_trackgen_internal.h:4492-4499` [read]) already separates "where to write" from "what to write"; only `nl`
and `si` are route-bound.

### 4.2 Two facts that shrink F4

1. **Roads are not scenery in roam.** Today side streets, plaza ring ribbons and the avenue's far
   carriageway are scenery meshes (`tg_net_emit_entry`, `tg_net_geo_plaza_roads`, `tg_av_emit_road`
   [survey]). In roam every street is drawn from the ROAD layer (3.6), so none of those emitters is ported.
2. **Procedural frontage is not needed on geo.** Real OSM buildings replace `tg_emit_street_wall` and
   `tg_city_emit_backrows`, which are the most span-bound emitters (superblock hash, `s_r15_store_last`
   sequence, neighbour caps) [survey].

### 4.3 Per-tile context and re-keying

```c
typedef struct {                /* the frame every emitter asks for, replaces tg_city_edge_frame */
    double px, pz, py;          /* point on the edge centreline at `along`                       */
    double tx, tz;              /* unit tangent (row direction)                                   */
    double nx, nz;              /* unit outward normal for `side`                                 */
    double half_carriage;       /* units                                                          */
    double kerb_y;
    int    edge;                /* tile-local edge index; key in RoamEdge.key                     */
} RoamFrame;

typedef struct TG_TileCtx {
    const RoamTile *road[9];    /* this tile's ROAD layer + its 8 neighbours (clearance, halo)    */
    int32_t  org_x, org_z;      /* tile origin, place frame                                       */
    uint32_t seed;              /* hash(place, tx, tz): every procedural pick keys off            */
                                /* hash(edge_key, row, side) instead, so it is tile-independent    */
    TG_Buf   blk[16];           /* one block per 64 m sub-block (<= 256 meshes each)               */
    size_t  *moff[16]; int nmesh[16]; int maxmesh;
    TG_GuardMarks guard;        /* replaces s_guard_ex_* (td5_tg_guard.c:78-86)                    */
    TG_PaveMarks  pave;         /* replaces s_pave_mark_* (:333-341)                               */
    TG_SceneryStats stats;      /* all class-(c) counters                                          */
    RoamSideMemo *sw;           /* (edge, side) -> pavement width, wall offset (was s_geo_sw_w)    */
    const void *ambient_surf;   /* was s_geop_surf; same for the other ambient pointers            */
} TG_TileCtx;

int roam_edge_frame(const TG_TileCtx *c, int edge, int side, double along, RoamFrame *out);
int roam_nearest_edge(const TG_TileCtx *c, double x, double z, double max_d,
                      int *edge, int *side, double *along, double *lateral);
```

Re-keying rules:
- `si` (a span on the route) becomes `(edge_key, side, along)`, `side` relative to the edge's row direction.
  Emitters that step "span by span" step by ribbon row (same 1500 spacing, so their constants keep meaning).
- `si +- 1` neighbour tests (sidewalk end caps, facade neighbours) become edge adjacency at the cluster: an
  edge end that meets a junction patch gets the corner treatment, a join node gets a continuous slab.
- Ownership: anchor point decides (building centroid, footway segment midpoint, signal node, sidewalk quad
  midpoint, tree position). Clearance tests read the 3x3 ROAD neighbourhood, so a scenery tile build waits
  until its 8 neighbours' ROAD tiles exist (the request queue orders it).
- Guard: `tg_guard_validate_entry` (span-window based, `td5_trackgen.c:4463` [read]) is replaced in roam by
  one rule against the explicit ROAD layer: no scenery triangle may overlap a drivable primitive's XZ
  footprint unless it is flagged as on-road furniture.
- Strip byte-identity: the strip path keeps calling its emitters exactly as today. Shared helpers get a
  context parameter whose strip instance wraps the current globals, so level091 and the synthetic gate stay
  byte-identical in every F4 round. Roam gets its own dispatcher (`td5_roam_scenery.c`) instead of
  `tg_scenery_entry`.
- Textures: no new pages per tile (3.8). Street-name plates (mast arms) need one plate cell per street name;
  build the plate atlas once at roam start for every name in the resident place window (La Plata: 187
  distinct names in 1,937 named ways, `td5_geo_roads.h:239-241` [read]).

### 4.4 F4 rounds (ranked by value for the city look)

| Round | Content | Effort | Size [survey est] |
|---|---|---|---|
| **F4a** | `TG_TileCtx`, `roam_edge_frame`, scenery dispatcher + sub-block publish; real OSM buildings (`tg_geo_emit_one` with its 4 span uses swapped for `roam_nearest_edge`, apron, landmark prefabs), OSM footpaths (already world segments, `si` is only a bucket), prefab landmarks, plaza lawns + plaza trees (world lattice + canopy raster), the roam guard rule; re-bucket `s_gb` by tile | **high** (it fixes the context every later round uses) | 1,000-1,300 LOC |
| **F4b** | Sidewalk slab, kerb, fence and the junction corner pieces along (edge, side): generalise `tg_net_emit_fw_follow` (`td5_tg_network.c:3277`, already a per-edge emitter) and port `tg_block_emit_intersection/_arm/_arm_follow` as cluster-level emitters that follow the 1.3 patch outline; pavement widths from `td5_geo_sw_resolve` per (edge, side) | **high** | 1,200-1,600 LOC |
| **F4c** | Furniture on (edge, side, along): signals + mast arms + name plates, bus stops, zebra crossings at clusters, street lamps, infra props; plan rows change to `{edge, side, along, x, z}`, emitters only swap the frame | **medium** | 900-1,200 LOC |
| **F4d** (optional) | Verge trees, tree lines, park and slope flora with a world spatial hash replacing `tg_r12_flora_accept`; guardrails only on trunk/bridge edges | **medium** | 1,000-1,500 LOC (procedural frontage excluded) |

F4a + F4b give the look Mariano will judge ("buildings and sidewalks"); F4c brings the signals that F8 traffic
needs anyway. Every round needs the threading hazards cleared for the functions it touches: function-local static
scratch arrays (`td5_tg_prefab.c:86-88`, `td5_tg_guard.c:810`, `:1574-1691`, `td5_tg_network.c:492`, `:1376`,
`:1429`, `:1877-1879`, `:2515`, `td5_tg_city.c:2332`, `:2462`, `td5_tg_streets.c:4595`) and ambient pointers
[survey]. Comments in `td5_tg_furniture.c:1335` and `td5_tg_city.c:1662` say the entry loop runs on worker
threads; the code is serial (`td5_trackgen.c:4724-4725`, `:5771-5773`) [survey]. Do not take those comments as
proof of thread safety.

Aside found on the way (strip path, not roam): the generator's per-entry budget is
`TG_MAX_MESHES_PER_ENTRY = TD5_TG_SPANS_PER_ENTRY * 96` = 384 (`td5_trackgen_internal.h:6225` [read]), while the
stream ingest rejects any block with more than 256 meshes as a whole (`td5_track.c:845` with
`TD5_SCN_SLOTS_PER_ENTRY = 256`, `:720` [read]); I saw no clamp in the assembly (`td5_trackgen.c:4467-4485`
[read]). An entry over 256 meshes would lose ALL its scenery. Not verified at runtime: worth one log line
(`nmesh > 256`) in the next strip round.

---

## 6. F1 task list (independent sub-tasks)

Interfaces fixed first (in the F1 brief): `RoamWay` (parsed way, place frame, with way id), `RoamGraph`
(nodes, edges, clusters), `RoamTile` (1.6), `TD5_RoamHit` + `td5_roam_locate` (1.9), ROAMTILE format (1.7).
Everything lives in new files (`td5_roam_world.c`, `td5_roam_junction.c`, `td5_roam_locate.c`,
`td5_roam_cache.c`) except T6's refactor.

| # | Task | Depends on | Effort |
|---|---|---|---|
| T1 | **Cache reader**: INDEX.JSON tile selection for a window, parse each cache ROADS tile once into a compact session table (way id, all attributes, place-frame points, 10-unit node keys), re-entrant record parser lifted from `td5_geo_roads.c`; one roam-only lane-count function (strip copies untouched). Census line: ways, points, cache tiles read, ms | - | low |
| T2 | **Raster window**: HEIGHT.R16 / WATER.R8 window reads via `gr_raster_read_window` (export it), bilinear + exaggeration + water rule identical to `td5_geo_height`; 9-window LRU | - | low |
| T3 | **Graph + junction clusters + patch polygons** (1.2, 1.3): node snap, edges, degree-2 joins and tapers, clustering with the 30 m / 60 m rules, arm cut rows, corners, fillets, fan + ear-clip fallback, triangle-edge split at 20,000 units, the swept-width check of 2.5 rule 4
(`ROAM_SWEPT_FAIL`). Offline dump of every cluster polygon to JSON plus a small Python SVG viewer under `_archive/roam/F1/`. Census: clusters by arm count, fallbacks, max diameter, rejected unions | T1 | **high** |
| T4 | **Ribbons + heights** (1.4, 1.5): resample with `gr_resample`, row 0 at the smaller node key, 1 km edge split, raster profile, smoothing, end blend, pure `roam_grade_solve` (copy of the three-pass rule), bridge/tunnel lift with `ROAM_RAMP_SHORT`, explicit integer quantisation, quad ownership by midpoint | T1, T2 (T3 for cut rows; can start on the plain-edge case) | medium |
| T5 | **Tile assembly + ROAMTILE writer/reader + determinism harness**: ROAMGRID.JSON (rotated casco lattice, 1.8), per-tile arena, sorted iteration, section table, input hash, trailer; dev CLI `--RoamBuildTiles=x0,z0,x1,z1` that writes files and exits; Python checker that builds twice and in reversed order (and once on a worker thread) and compares bytes | T3, T4 | medium |
| T6 | **Locator** (1.9) + the `triangle_height` / `diagonal_cross` / `get_quad_vertices` vertex-table parameter refactor in `td5_track.c` (strip byte-identical: synthetic gate MODELS hash + level091 identity + selftest smoke). Dev self-test: every quad and tri centroid locates to itself with `inside = 1`; a probe 0.5 m outside every boundary edge returns a push pointing back in; the strip-mirror offline check (every type-1 span's height at 9 points equals `td5_track_compute_contact_height_with_normal`) | T5 (refactor part has no dependency) | medium |
| T7 | **Coverage + seam audit** (Python over ROAMTILE files): every drivable way in the casco box owns >= 1 quad or is inside a cluster; every border row vertex identical in both tiles; no boundary edge shorter than 1 cm; no open boundary between a ribbon and its patch | T5 | low |
| T8 | **Dev viewer**: in free-cam, draw resident ROAD tiles through the F12 collision overlay (boundary white, interior cyan, the locator's prim under the camera yellow); `geo_tour.ps1 -Extra @{TD5RE_ROAM_VIEW=1}` poses on 10 clusters (5 plazas, 3 divided-avenue crossings, 2 bridges) | T5 | low |

Acceptance for F1 as written in the plan, made concrete: T5's checker reports identical bytes (twice, reversed
order, worker thread); T7 reports 0 uncovered drivable ways in the box and 0 seam mismatches; T8's PNGs are
looked at by the orchestrator, not only listed.

---

## 5. Risks and unsound points

### 5.1 Points in the plan / Decisions table that are technically unsound or overstated

1. **"GEO_MULTI_JUNCTION.md already solves the shapes"** (plan, Road world). It does not. That work is a
   rule set on the (span, side) mouth table of ONE route: a V of two streets on one side, re-aiming a kinked
   arm, a plaza yield. It says itself that more than two streets on one side and converging streets are
   refused and that a multi-valued table is "a larger refactor touching every emitter"
   (`GEO_MULTI_JUNCTION.md:63-68` [read]). The N-arm patch of 1.3 is new code and the hardest F1 sub-task.
2. **"Origin rebasing ... needed only from F9."** F0a's casco corners in the place frame
   (`x = (lon-lon0)*m_per_deg_lon*430`, `z = (lat-lat0)*m_per_deg_lat*430`, north +z) are
   (1,078,706, 5,374,176), (2,582,492, **7,019,046**), (**4,255,470**, 5,615,132) and (2,731,713, 3,880,755).
   The north corner is 84 % of the 8.39 M 24.8 limit. Any int32 24.8 expression of the form `a + b` over two
   positions (midpoints, sums before a divide) overflows at 4.19 M, and the casco is past that on both axes.
   Set `world_origin` = the spawn tile's origin at roam start from F2 (cheap: tiles are stored tile-local
   anyway; the casco is 7.4 km corner to corner, so every point is then within +-3.2 M units of the origin);
   dynamic shifting can wait for F9.
3. **"Edges crossing a tile border are owned by the tile holding their midpoint."** An edge is a way piece
   between junctions (La Plata blocks are ~120 m, rural edges are km), so the owning tile's mesh would reach
   far outside its square, the cull sphere would be wrong and a tile could not be built from its halo. Own by
   **quad** (1.4 / 0.2). F0a measured 1,934 border-crossing segments on the casco-aligned 256 m lattice.
4. **"Tile content is a pure function of (place, tile id, spec, exe)."** Right as an invariant; wrong as a
   cache key. Keying on the exe invalidates every cached tile on every build. Key on a hand-bumped
   `ROAM_RULES_VERSION` plus the input hash (1.7), and keep the "build twice / two orders, same bytes" test as
   the guard that the rule version was bumped when needed.
5. **Casco size (settled by F0a).** The plan's "~4 x 4 km" is a 5.2 km diamond of 27 km2 rotated -42.13 deg:
   484 km drivable, 2,170 ways (1,883 one-way), 462 tiles. F2 "all tiles loaded at once" is then ~25 MB of ROAD
   layer [est], fine. Update the plan's wording so acceptance numbers refer to the polygon.
6. **"Scenery: same detail as races" from day one of F4.** The emitters are single-instance and route-keyed
   (section 4); "same detail" is reachable only through 3+ rounds. The plan already says F4 may split; the
   split is not optional.
7. **"Racer-only collision layer, traffic passes through."** Fine, and cheap today: traffic slots >= 6 skip
   the wall resolver entirely (`td5_track.c:2511` [read]). But it also means **traffic has no road walls at all**
   in roam: traffic must be kept on its lane by the F8 lane follower, not by walls.

### 5.2 Top risks

1. **Junction patch robustness** (F1). 6,559 shared nodes and 1,639 real junctions (degree >= 3) in F0a's
   north-up primary box (a superset of the casco polygon), 87 roundabout ways, divided avenues meeting diagonals. Bad shapes are walls in the middle of a crossing
   or holes a car falls into. Mitigation: a census in F1 (clusters by arm count, fan fallbacks, ear-clip
   fallbacks, max cluster diameter, any non-watertight seam) and a free-cam tour of the 30 worst clusters.
2. **Walls** (F2). The forward-track history says a per-tick nearest-edge push "WEDGES a car at a sharp fork
   bend" (`td5_track.c:1754-1760` [read]). 2.5 gives the probable mechanism and four counter-rules, but none of
   that is proven until the F2 corner test runs. Roam has thousands of 90-degree corners; test before streaming.
3. **Height seams at junctions on slopes**: the fan design gives exact seams, but large `H_c` vs arm-row
   differences make a crest in the patch. Log the max per-cluster height spread; clamp the arm blend if
   spread > 0.5 m.
4. **Builder determinism across threads**: FP rounding env (1.10), sorted iteration, explicit rounding.
   The F1 "same bytes in two orders" test must also run once on the worker thread vs once on main.
5. **Emitter port size** (F4): see section 4.
6. **Physics parity** (F2): Stage A proves the provider; Stage B differences are builder-made and must be
   reported, not hidden (the strip pins y0 = 0 and adds a synthetic detail octave the roam builder does not).
7. **Raster I/O cost**: HEIGHT.R16 is 346 MB; read windows by seek (`gr_raster_read_window`), never slurp.
   Per road tile build: one 72x72-cell window + halo (~1 km) = ~1.4 MB read [est]. Cache the last 9 windows.
8. **cJSON parse cost**: a road tile needs 1-4 whole cache tiles of ROADS (largest any-layer tile 6.3 MB,
   BUILDINGS). Parse each cache tile once per session into a compact in-memory road table shared read-only by
   all builds (main thread, or under one lock), not once per roam tile.
