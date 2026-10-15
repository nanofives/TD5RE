# F0a: La Plata casco urbano, road and building numbers

Read-only measurement for GEO FREE ROAM (`docs/plans/GEO_FREE_ROAM.md`, phase F0).
Nothing in the repo was edited. Scratch and output live in `_archive/roam/F0a/`.

## Command

```
cd C:\Users\maria\Desktop\Proyectos\TD5RE\_archive\roam\F0a
python -I measure.py
python -I explore.py   # avenue name-tag extents + top names (stdout only)
```

Stdlib only. Inputs: `re/assets/geo/la_plata_partido/PLACE.JSON`, `tiles/ROADS_*`, `tiles/BUILDINGS_*`, `tiles/SIGNALS_*`.

## Projection (lat/lon to world)

- `PLACE.JSON` `projection`: kind `local_tangent_plane_wgs84`, lat0 -35.0352183, lon0 -58.0215769, m/deg lat 110941.220, m/deg lon 91249.039, units_per_metre 430.0, rotation 0, offset (0, 0).
- Formula (`re/tools/geo_common.py:184-187` and `:214-216`, rotation 0, offset 0):
  `x = (lon - lon0) * m_per_deg_lon * 430`, `z = (lat - lat0) * m_per_deg_lat * 430`.
  North is +z.
- Checked against the file: every ROADS point is re-projected from its `latlon` and compared with its stored `x,z`. Max residual 0.000016 m over all roads points; points off by more than 0.5 m: 0.

## Casco polygon

Avenue name-tag extents (from `ROADS` `name` tags, every way that matches; `explore.py` prints the same):

| Name tag | ways | lat min | lat max | lon min | lon max |
|---|---:|---:|---:|---:|---:|
| Av 122 | 51 | -34.930938 | -34.884897 | -57.956803 | -57.894777 |
| Av 31 | 78 | -34.956446 | -34.898436 | -58.026578 | -57.948611 |
| Av 32 | 74 | -34.957107 | -34.889004 | -58.031019 | -57.957080 |
| Av 72 | 59 | -34.965164 | -34.917512 | -57.965354 | -57.913122 |

- **Primary box (the one the orchestrator gave):** lat -34.955 .. -34.885, lon -57.995 .. -57.905. Size 3,531,338 x 3,339,331 world units = 8.21 x 7.77 km.
- **Alternative box (sensitivity):** union of the four avenue extents above, lat -34.965164 .. -34.884897, lon -58.031019 .. -57.894777. It is an upper bound: each avenue way runs past the ring, so this box is larger than the ring.
- **Size check:** the plan says the casco is about 4 x 4 km. The primary box is about 8.2 x 7.8 km. Either the plan's figure is a half-width or the box is too big. The numbers below use the primary box, and the alternative box is shown next to it. This needs a decision from Mariano.
- Ways are kept when any vertex is inside the box. Lengths and per-tile counts use segments whose midpoint is inside the box, so a way that crosses the edge is cut at its midpoints, not exactly.

## Data loaded

- ROADS: 413 tile files, 17,310 unique ways (duplicate ids skipped: 0; points vs latlon length mismatches: 0; ways with no latlon: 0).
- BUILDINGS: 453 tile files, 516,358 unique footprints (duplicate ids skipped: 0). All BUILDINGS files together: 219.8 MB on disk.
- SIGNALS: 134 tile files. `signals[]` (what the game reads) = 1,036; `nodes[]` (other kinds) = 2,337.

## 1. Drivable ways

Filter. A way is drivable when it is in `ROADS` `roads[]`. The writer never puts `HIGHWAY_NONDRIVABLE` values there, so the filter is the writer's, and a second check counts any leaked value.
- Writer: `re/tools/geo_fetch.py:155-158` (set), `:1259-1264` (skip), `:1319-1322` (append to `roads`).
- Reader: `td5mod/src/td5re/td5_geo_roads.c` keeps every `roads[]` entry with at least 2 points (`:530-580`). `geo_roads_class` (`:109-124`) maps classes to `RC_*`, and any other class becomes `RC_UNKNOWN` but is still kept.
- Router: `td5mod/src/td5re/td5_geo_route.c:134-143` prices the classes in `ROUTER_CLASSES`.
- Non-drivable values found in the ROADS files: 0.

Primary box: **1,094.5 km** of drivable way (midpoint rule), 4,492 ways with a vertex inside, of which 2,790 one-way.
Alternative box: 1,742.3 km, 6,522 ways, 3,084 one-way.

| Highway class (primary box) | ways | km | one-way ways |
|---|---:|---:|---:|
| residential | 2,128 | 723.18 | 1,147 |
| secondary | 709 | 128.43 | 660 |
| service | 629 | 76.50 | 136 |
| primary | 457 | 69.48 | 426 |
| trunk | 250 | 41.39 | 205 |
| tertiary | 190 | 40.67 | 127 |
| unclassified | 32 | 7.93 | 17 |
| trunk_link | 38 | 2.64 | 38 |
| primary_link | 23 | 1.98 | 23 |
| living_street | 23 | 1.89 | 0 |
| secondary_link | 8 | 0.23 | 8 |
| tertiary_link | 4 | 0.14 | 3 |
| bus_stop | 1 | 0.04 | 0 |

## 2. Junctions and rings

A junction is an OSM node (exact 7-decimal `latlon`) that is a vertex of 2 or more distinct drivable ways. Degree = number of distinct ways. Only nodes inside the box count, but degrees use all ways in the file.

| Degree | Primary box | Alternative box |
|---|---:|---:|
| 2 (plain join, 2 ways) | 4,920 | 8,037 |
| 3 | 948 | 1,312 |
| 4 | 643 | 767 |
| 5 | 32 | 40 |
| 6+ | 16 | 16 |
| **all shared nodes** | 6,559 | 10,172 |

- Closed ways (first point = last point), primary box: 17.
- Ways tagged `junction=roundabout`, primary box: 87.
- Plaza or ring-road loops in OSM are closed ways, so the closed count is the plaza and ring candidate count.

## 3. Tile sizes (world-aligned square grid, tiles overlapping the primary box)

Ownership: a segment belongs to the tile of its midpoint, a junction to its node position, a building to its footprint centroid, a signal to its position. Only features inside the primary box are counted, and all grid tiles are listed (zeros included). `crossing_edges` = segments whose two endpoints are in different tiles, counted to the owner tile.

### 128 m tiles

- Tiles overlapping the box: **4092** (66 x 62). Tiles with zero drivable km: 367.
- Segments in box: 17,308. Segments crossing a tile border: **8,255** (47.69 % of segments).

| Per tile | min | median | p95 | max | sum | tiles |
|---|---:|---:|---:|---:|---:|---:|
| drivable km | 0.000 | 0.272 | 0.551 | 2.346 | 1,094.499 | 4092 |
| way pieces (runs inside tile) | 0 | 2 | 7 | 20 | 10,886 | 4092 |
| junction nodes | 0 | 1 | 5 | 17 | 6,559 | 4092 |
| buildings (footprints) | 0 | 41 | 87 | 129 | 164,773 | 4092 |
| building vertices (raw) | 0 | 249 | 490 | 700 | 963,303 | 4092 |
| traffic signals | 0 | 0 | 2 | 7 | 737 | 4092 |
| crossing edges (owner) | 0 | 2 | 5 | 10 | 8,255 | 4092 |

Per-tile rows: `tiles_128m.csv`.

### 256 m tiles

- Tiles overlapping the box: **1056** (33 x 32). Tiles with zero drivable km: 29.
- Segments in box: 17,308. Segments crossing a tile border: **5,040** (29.12 % of segments).

| Per tile | min | median | p95 | max | sum | tiles |
|---|---:|---:|---:|---:|---:|---:|
| drivable km | 0.000 | 1.112 | 1.678 | 2.506 | 1,094.499 | 1056 |
| way pieces (runs inside tile) | 0 | 6 | 18 | 40 | 8,248 | 1056 |
| junction nodes | 0 | 5 | 15 | 33 | 6,559 | 1056 |
| buildings (footprints) | 0 | 160 | 310 | 407 | 164,773 | 1056 |
| building vertices (raw) | 0 | 960 | 1,752 | 2,269 | 963,303 | 1056 |
| traffic signals | 0 | 0 | 4 | 17 | 737 | 1056 |
| crossing edges (owner) | 0 | 5 | 9 | 14 | 5,040 | 1056 |

Per-tile rows: `tiles_256m.csv`.

### 512 m tiles

- Tiles overlapping the box: **272** (17 x 16). Tiles with zero drivable km: 0.
- Segments in box: 17,308. Segments crossing a tile border: **2,740** (15.83 % of segments).

| Per tile | min | median | p95 | max | sum | tiles |
|---|---:|---:|---:|---:|---:|---:|
| drivable km | 0.333 | 4.363 | 5.871 | 6.953 | 1,094.499 | 272 |
| way pieces (runs inside tile) | 1 | 22 | 50 | 75 | 6,544 | 272 |
| junction nodes | 0 | 24 | 47 | 67 | 6,559 | 272 |
| buildings (footprints) | 0 | 616 | 1,150 | 1,385 | 164,773 | 272 |
| building vertices (raw) | 0 | 3,685 | 6,555 | 7,776 | 963,303 | 272 |
| traffic signals | 0 | 1 | 13 | 28 | 737 | 272 |
| crossing edges (owner) | 1 | 10 | 18 | 23 | 2,740 | 272 |

Per-tile rows: `tiles_512m.csv`.

## 4. Bridges, tunnels, layers inside the box

| Item (primary box) | count |
|---|---:|
| ways with `bridge=yes` | 37 |
| ways with `tunnel=yes` | 10 |
| ways with `layer != 0` | 45 |

- bridge kinds: `{'yes': 37}`; classes: `{'residential': 13, 'service': 7, 'primary': 12, 'secondary': 4, 'tertiary': 1}`
- tunnel kinds: `{'yes': 2, 'building_passage': 8}`; classes: `{'trunk': 2, 'service': 8}`
- layer values: `{1: 37, -1: 8}`
- Source: `geo_fetch.py:1330-1339` (`bridge`, `tunnel`, `layer` read from the way). Only `roads[]` is counted here, so a bridge that is not a drivable way (none expected) is not in this table.

## 5. Building data volume (primary box)

- Footprints inside the box: **164,773**.
- Vertices (raw `points`, closing point included): **963,303**.
- Source JSON bytes of those records, measured on the raw file text (`array_element_bytes`): **69,351,769 bytes (69.35 MB)**.
- Alternative box: 236,896 footprints, 1,374,880 vertices, 100.27 MB.

## 6. Ambiguities and limits

- **Casco size.** The plan says about 4 x 4 km. The box given is about 8.2 x 7.8 km (see the Casco section). Both boxes are reported.
- **Midpoint rule.** Km and crossing counts use segment midpoints, so a segment cut by a border is counted once, in the tile of its midpoint. A way that leaves the box keeps its inside segments only.
- **Degree-2 nodes.** A node shared by exactly two ways is often just a split in OSM, not a real junction. The 3/4/5/6+ rows are the real junctions. The degree-2 row is shown for completeness.
- **Signals.** Only `signals[]` is counted as traffic signals (the game reads that key, `td5_geo_signals.c:275`). `nodes[]` holds other kinds (crossings, bus stops, calming) and is reported as a count in the data section only.
- **Building vertices.** Raw count, including the closing point of each ring. Unique vertices are about one fewer per footprint.
- **Bytes.** Measured on the raw file, per element, with the same bytes the file holds (UTF-8). The records are not re-serialised.
- **Duplicates.** The loader keeps the first copy of any repeated id; the counts above show how many were skipped (0 expected).
- **Projection.** Verified against the file, not assumed. Residual is in the Projection section.
- **`bus_stop` in roads[].** One way with `class = bus_stop` is in the primary box. The writer's non-drivable set does not list it, so it is kept as a drivable road (class RC_UNKNOWN in the reader). It is 0.04 km, so the effect on the totals is nil, but the drivable filter for F1 should decide on it explicitly.
- **`tunnel=building_passage`.** 8 of the 10 tunnel ways are archways through buildings (`tunnel_kind`), not bores. Only `tunnel=yes` (2 ways) is a real tunnel. The F1 grade-separation rules should key on the value, not the boolean.
- **Plaza and ring loops.** Closed-way and roundabout counts are what the data gives. Whether a closed way is a plaza ring the game should treat as a loop is not decided here.

## Raw results

`results.json` holds every number above, in both boxes, for the orchestrator to re-read.

<!-- casco-poly:start -->
## Casco polygon (rotated, from the ring avenues)

Run: `python -I measure_poly.py` (writes `casco_poly.json`, `tiles_256m_poly_*.csv`, and this section).
The earlier north-up sections above stay as they were. This section replaces them for the casco.

### Corners

Each corner is the closest approach between two named avenue ways (`Avenida N` / `Av N` tags, both ways of each pair). The corner is the midpoint of the closest pair. Corners are in ring order.

| Corner (ring pair) | lat | lon | world x | world z |
|---|---:|---:|---:|---:|
| Av 31 x Av 32 | -34.922563 | -57.994085 | 1,078,706 | 5,374,176 |
| Av 32 x Av 122 | -34.888083 | -57.955759 | 2,582,492 | 7,019,046 |
| Av 122 x Av 72 | -34.917512 | -57.913122 | 4,255,470 | 5,615,132 |
| Av 72 x Av 31 | -34.953869 | -57.951956 | 2,731,713 | 3,880,755 |

Closest-approach distance per pair (m), and other candidate clusters within 500 m:

- Av 31 x Av 32: best 0.0 m; other clusters: 123.7 m at (-34.923922, -57.993360), 276.4 m at (-34.922198, -57.992355), 276.6 m at (-34.924439, -57.994996)
- Av 32 x Av 122: best 363.785 m; other clusters: 421.5 m at (-34.887144, -57.957055), 481.1 m at (-34.889040, -57.954464)
- Av 122 x Av 72: best 0.0 m; other clusters: 298.3 m at (-34.918868, -57.912689), 298.5 m at (-34.917965, -57.914672), 303.2 m at (-34.916615, -57.914437)
- Av 72 x Av 31: best 0.0 m; other clusters: 133.1 m at (-34.953422, -57.953662), 266.4 m at (-34.952498, -57.951662), 272.4 m at (-34.954744, -57.954165)

### Geometry check (avenue axes, axial mean of segment directions)

- axial_bearing_deg (mod 180, atan2(dz,dx), 0 = east, 90 = north): Av 31 137.9, Av 32 48.0, Av 72 48.1, Av 122 138.0
- angle 31 vs 122 (deg): 0.11
- angle 32 vs 72 (deg): 0.08
- angle 31 vs 32 (deg): 89.90
- angle 32 vs 122 (deg): 89.99

### Side lengths and area

- Sides (m): Av 31-Av 32 5,183.0, Av 32-Av 122 5,079.1, Av 122-Av 72 5,369.0, Av 72-Av 31 5,180.7
- Area: 27.057 km²
- Shoelace area on the world frame, divided by 430²; side lengths from world distances / 430.

### Measurements clipped to the polygon (sections 1-5)

- Drivable km (midpoint rule): **484.45 km**; ways touching the polygon: 2,170; one-way: 1,883.

| Highway class | ways | km | one-way |
|---|---:|---:|---:|
| residential | 835 | 301.49 | 765 |
| secondary | 498 | 77.37 | 468 |
| primary | 347 | 44.41 | 346 |
| tertiary | 82 | 19.06 | 70 |
| service | 233 | 16.70 | 81 |
| trunk | 102 | 16.17 | 91 |
| unclassified | 28 | 7.13 | 17 |
| trunk_link | 32 | 1.68 | 32 |
| primary_link | 4 | 0.20 | 4 |
| secondary_link | 6 | 0.15 | 6 |
| tertiary_link | 3 | 0.09 | 3 |

| Junction degree | nodes |
|---|---:|
| 2 | 1,938 |
| 3 | 490 |
| 4 | 425 |
| 5 | 21 |
| 6+ | 15 |
| all shared | 2,889 |

- Closed ways: 3; roundabout ways: 37.
- Bridges: 1 (kinds {'yes': 1}); tunnels: 9 (kinds {'yes': 1, 'building_passage': 8}); layer != 0: 7 (values {-1: 6, 1: 1}).
- Buildings: 85,034 footprints, 491,072 raw vertices, 35,466,351 source bytes (35.47 MB).
- Traffic signals (`signals[]`): 535; other nodes (`nodes[]`): 931.

### 256 m tiles: north-up vs rotated to the casco axes

Rotation used for the rotated grid: **-42.13°** (-0.7354 rad), the axis of Av 31 folded into (-45°, 45°]. For comparison the strip route uses rotation_rad -0.8513 (-48.8°), which is a different frame and is not used here.
Tiles are those that intersect the polygon. Features count only inside the polygon, owned by the tile of their midpoint, centroid or position.

| Per tile (256 m) | north-up min | north-up median | north-up p95 | north-up max | rotated min | rotated median | rotated p95 | rotated max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| drivable km | 0.000 | 1.115 | 1.702 | 1.988 | 0.000 | 1.111 | 1.786 | 2.219 |
| way pieces | 0 | 6 | 20 | 40 | 0 | 6 | 20 | 38 |
| junction nodes | 0 | 5 | 15 | 29 | 0 | 6 | 15 | 35 |
| buildings | 0 | 203 | 333 | 407 | 0 | 205 | 336 | 407 |
| building vertices | 0 | 1175 | 1873 | 2269 | 0 | 1190 | 1887 | 2258 |
| traffic signals | 0 | 0 | 6 | 17 | 0 | 0 | 5 | 11 |
| crossing edges | 0 | 5 | 10 | 14 | 0 | 4 | 8 | 12 |

| Grid | tiles intersecting polygon | tiles with 0 km | border-crossing segments | segments in polygon |
|---|---:|---:|---:|---:|
| 256 m north-up | 473 | 23 | 2,216 | 8,074 |
| 256 m rotated -42.13° | 462 | 22 | 1,934 | 8,074 |

Per-tile rows: `tiles_256m_poly_northup.csv`, `tiles_256m_poly_rotated.csv`.

- **Av 32 x Av 122 has no shared node.** The closest approach is 364 m, so that corner is the midpoint of a 364 m gap, and its true position is uncertain by roughly 180 m. The other three corners are shared-node intersections (0.0 m). The Av 122 ways near that corner should be checked in the data before F1 uses this polygon.
Notes: the polygon is convex, so a tile intersects it by a separating-axis test on the tile's own frame. The rotated grid's tiles are axis-aligned to the casco, so tile count is close to area / tile area with a boundary fringe.

