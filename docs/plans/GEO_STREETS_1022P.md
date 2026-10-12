# GEO STREETS AND SCENERY LEFTOVERS (round 1022 group P)

La Plata (slot 61 / level091), Mariano's route (`_route/ROUTE.JSON` MD5 `B5135EC4...`, fingerprint 7820EC8E, Diagonal 73,
772 nodes), code base master c520ccc8. Three items from the round 1021 leftovers. Every change is geo-only
(`tg_net_geo_active()` / `td5_geo_loaded()`), has a knob that restores master, and the synthetic slot 60 build is untouched.

## Item 1: the road that joins the main road (round 1021 item 15, Calle 54, spans 336..352)

### What is there (measured)

Three real ways meet at a Y node 12 m beside the route, at (488493,-57134): Calle 54 from the south-west (OSM way 287907813),
the primary_link from the west (51126236, which carries Diagonal 74) and the way that merges into the route (271454263,
Calle 54 again, 18.6k units long). The merging way reaches the route at its vertex (500538,-42989) and then shares the
route's carriageway for 17 spans to Avenida 13. The approach angle to the route's tangent is 37 degrees (my first estimate,
16, ignored that the route is curving there).

Why no street was ever drawn (all three are in `tg_geo_road_hits` / `tg_geo_arm_push`):

* the crossing rule needs the lateral offset to change sign; a merge never does;
* the way spends 11 of its 17 samples on the route, so `tg_geo_road_hits` calls it "the route itself" (-1) and drops it
  *together with the shared-carriageway departure it had found* (1008b);
* the Y node lies 5240 units past the kerb, further than the T-end rule's one span of slack.

### What changed

A new edge kind: a **frontage road**, `tg_net_geo_frontage` in `td5_tg_network.c`, run after the street arms and before the
plaza roads. It is the plaza roads' mitred ribbon (carriageway + raised footway, one mesh), laid along the real way's own
vertices (resampled every 1800 units):

* **qualifies**: a way (not bridge/tunnel/layer, not a service lane) with a sample inside the main kerb zone and the next one
  outside it, whose inner samples then keep inside the zone for 5 samples (21 m) without crossing to the other side. A way that
  crosses the route or ends at it is inside for two or three samples; a way that already owns a street arm is skipped;
* **where it runs**: from where it stops being free of every other road (paint test with a footway-wide margin) to the main
  pavement, never further out than the flat verge (`tg_verge_reach`, 24000 units: past it the ground rises and falls away from a
  level road; my first 30000 left 52 samples buried under far-band terrain);
* **merge taper**: over the last segment the carriageway narrows to a point on the outer edge of the main pavement (+300 clear),
  its inner edge running along that edge, so it is a merge and not a square-cut stub;
* **footway on the outer side only** (the side facing the route is the main road's own pavement): `fw_skip` on the edge;
* **continuation**: ways that touch a ribbon's open end vertex (<=1500 units) are laid too, 2 hops, so the Y is not a dead end
  (Calle 54 to the south-west, the primary_link);
* **height**: the ribbon never sinks below the ground skirt. The skirt of a span is the road edge height minus the span's own
  cross-section profile (`tg_ground_side`, the table `tg_emit_ground` reads); `tg_fr_skirt_y` evaluates it on the two bounding
  rows and the ribbon takes max(that + 45, the terrain envelope + 60) (`tg_net_fr_y`). The terrain envelope alone sat 22 units
  *under* the skirt at the far end (audit: "covered by terrain") and 49 over it at the near end;
* no ground conform (the plaza roads conform the terrain to their line; this one rides on the skirt instead, so every other
  emitter beside it reads the same terrain as before); paint `TG_WO_STREET` so later passes and the minimap treat it as a street;
* the start grid (`s->ni < tg_start_city_run()`) is skipped: its frontage is forced built (a ribbon at span 4 appeared and was
  removed).

NOT drivable (scenery, like the plaza roads and the opposite carriageway of an avenue): STRIP.DAT is untouched.

Result on his route (race.log `[NET/GEO FRONTAGE]`): 4 merge ribbons and 3 continuation ribbons (way 510 at owner span 343, way 466
the link, way 717 Calle 54 south-west, and the same mechanism at span 89 / 92, 643 and 665). Frames (`verify/geo_tour.ps1`,
`tour_f3_o1`): the Y with the merging diagonal tapering into the main pavement, the link road and Calle 54 laid out beside it.

| knob | default | effect when 0 |
|---|---|---|
| `TD5RE_GEO_FRONTAGE` | 1 | no frontage ribbons (master) |
| `TD5RE_GEO_FRONTAGE_DIAG` | 0 | 1 logs every transition and its rejection reason |

### Not done / known

* the far ends of the continuation ribbons end square (no further hop, no end cap); the 1500-unit end-vertex match misses a way
  that meets the ribbon mid-polyline;
* the taper assumes the way is straight across the taper (it interpolates between samples 1800 units apart);
* ribbons are not drivable and not in NETWORK.JSON's drivable list (`kind backstreet`, `drivable 0`), like the plaza roads.

## Item 2: the stray raised pavement arm near the Plaza Moreno approach

The frame named in the brief (round 1021 C `tour_after_i11a`) no longer exists, and "span ~30" is not Plaza Moreno (that is
spans ~250..262, picks e65 / e66). Two real defects were found there with the 1015 B audit (`verify/geo_r1015b_audit.py`) and
a return-address trace of the emitter, and both are fixed:

1. **Footway slab across a side street.** Mesh `e62 s50` is ONE quad of page 44 (pavement), 10000 x 1400 units, standing 71 above
   a cross-street carriageway at span ~250 (audit `city over city:101 dy=71`). Emitter (found by logging
   `__builtin_return_address(0)` in `tg_write_quad_mesh`): `tg_emit_geo_footpaths`, the real OSM footways. Its road gate clears the
   MAIN carriageway only. Fix: `tg_geo_footpaths_prepare` cuts a segment into <=700-unit samples and keeps only the runs that are
   not on a laid cross street (`tg_fp_on_street` = `tg_xstreet_covers`, centre and both flanks). A segment with no sample on a street
   is laid exactly as before. 6 segments cut on his route (`[GEO FOOTPATHS] [1022 P]` lines). Knob
   `TD5RE_GEO_FOOTPATH_STREET_CLIP=0` restores the old slabs.
2. **Kerb inside a neighbour's carriageway at a Y of the plaza ring road.** Edges 68, 60 and 69 (the Plaza Moreno ring roads) meet at
   (402683,47607); the footway slab's only overlap check tested the slab's CENTRE against the other carriageways, a whole slab
   from the other road, so the first section after the Y stood across the neighbour's tarmac (audit: `e65 s13`, 20 samples, `cross over
   cross:101 dy=149`, the largest overlap on the route). Fix: `tg_net_kerb_on_other_road` drops a footway section whose kerb line
   (start, middle, end) is more than 200 units inside another edge's carriageway. `TD5RE_GEO_FW_JOINT=0` restores master. Frames:
   `tour_i2b_p2a` before / after.

Audit (pavement samples inside a carriageway): master 86 of 30344, this branch 84 of 31160 (the frontage ribbons add pavement and
add 0 overlap).

## Item 3: the 345 m2 up-facing gap at span 252 (and 5 / 3 m2 at 253 / 258)

`verify/geo_r1021a_voidup.py` counts only up-facing triangles. At (376800,12593) (the 345 m2 component, 706 cells at 300 units) the
meshes present are the skirt (page 2), cross-street surface (page 101) and pavement (page 44), all wound DOWN by the audit's sign.
Frames over that exact point (`tour_v3_c1`, top-down at 7000 above the ground; `tour_v252u_u1/u2`) show lawn, a street and its
pavement: **no sky, nothing missing**. The 345 m2 component is harmless: the engine draws those triangles (so the "up" sign taken from
the road page is wrong for them: the left-hand-frame mirror of a few emitters) and the audit's number is a false positive. NOT
changed.

The 5 m2 (253, at (377850,23843)) and 3 m2 (258, (385350,14243)) components ARE visible: a flat light-blue (clear-colour) triangle
between the avenue's last pavement corner and the lawn (`tour_v252u_u2`). Tried and ruled out as the cause (component still there):
`TD5RE_R22_SKIP_DEGEN=0` (degenerate slabs), `TD5RE_R7_GUARD=0` (the on-road guard). Not fixed; the `geo_r1021a_voidup.py --min 2` list
for spans 245..262 is `252 343.6, 253 4.9, 258 3.4` identical before and after this branch.

## Tools / traps

* `td5mod/src/td5re/fastbuild.bat` is NOT committed (a scratch copy of `build_standalone.bat` that compiles only `%FASTSRCS%`): it
  cut an edit-test loop from 4 min to ~40 s. `build_standalone.bat` recompiles every module every time.
* `geo_tour.ps1` poses: `@span,lateral,height,yaw,pitch` has +lateral to the RIGHT in the frame's eyes but "span 343" is
  the strip index, not the HUD's SPAN; the HUD line is the CAR's span. World poses are `x,y,z,yawdeg,pitchdeg` with y = ground
  (mesh) y + the height above it (ground at the Y node 8513 -> y 17500 gives a 9000 high top-down).
* `TD5RE_AUTOTRACK_START_CITY` is not "on" in the geo tour; use `tg_start_city_run()` directly.
* `python -` heredocs get `/tmp/...` verbatim (C:\tmp\...): pass `$(cygpath -m /tmp)`.

## Gates

All on the exe `build_all.bat` produced (dev + release OK).

* **Knob-off identity on his route** (`verify/geo_tags_identity.ps1 -Arm geo`, `TD5RE_GEO_FRONTAGE=0 TD5RE_GEO_FW_JOINT=0
  TD5RE_GEO_FOOTPATH_STREET_CLIP=0`): MODELS.DAT 8122820 B `C0E01FE8D4A6F99F7904F5A1B672F9C8`, STRIP 83340 B `208386E6...`, TEXTURES
  1655200 B `FB1941C7...` = the master exe's own run in the same session (`td5re_master.exe`, same hashes).
* **Default**: MODELS.DAT 8155136 B `69CEBFDE9B0672716515A9476C90AE31`; STRIP and TEXTURES identical to master (no collision, AI or
  texture-page data moved).
* **Synthetic** (slot 60, seed 99991, SELECTED.TXT moved aside and restored): MODELS.DAT 12772392 B `E2F1F33C221D61CAB0EFF701484011F0`,
  STRIP `0641EDB7...`, TEXTURES `5B4E005A...`: identical to master.
* **Census** (`[NET/GEO] real street census`): 157 arms considered, 52 accepted (street 39, avenue 13) before and after; `[REAL FORK]`
  lines identical (md5 of the block equal): 2 driveable forks, 434 corridor spans, F=26 / F=429.
* **Selftest** `scripts/selftest.ps1 -Suite full`, three runs on a busy machine (three other children running): 63/1/0 (step 50
  chaos-coop-4seat, peak jump 8715 > 8000), 63/1/0 (step 28 race-r2-stress-max, peak jump 9358 > 8000), **64/0/0**. The WARN moves
  between steps and is the load-dependent peak-jump check on 8x races; none of those steps builds a geo level.
* `build_all.bat`: dev + release OK, structure lint OK (extern 3/3, game.h includers 23/23, warnings 84/84).
* Frames looked at (all `verify/geo_tour.ps1`, level REUSED or BUILT streamed): `tour_f3_o1` / `tour_fin_o1` (the Y at spans 336..352),
  `tour_f2_y1/y3/y4`, `tour_i2b_p2a/p2c` (plaza ring Y before / after), `tour_v3_c1` (footway across the side street before /
  after), `tour_v252u_u2` (the 253 hole), `tour_f5_t1/t3` (frontage ribbon at span 643).
