# GEO CROSSING STREETS, JUNCTIONS AND PAVEMENTS (round 1021 group B)

La Plata (slot 61 / level091), Mariano's route of 2026-10-11 19:56 (`_route/ROUTE.JSON` MD5 `B5135EC4...`,
fingerprint 7820EC8E, Diagonal 73, 772 nodes), code base master 70375f9e. Items 2, 5, 7, 8, 10, 15, 17 of his
21 picks. Every change is geo-only (`tg_geo_city_active()` / `td5_geo_loaded()`), has a knob that restores
master, and the synthetic slot 60 build is untouched.

All 8 picks were decoded on a rebuild (MODELS.DAT `(entry, slot)` + MESHTAG.BIN) and match the mesh header
(vertex count, command count, centre) of the picks in `_archive/round1021/picks_from_log.txt`. NOTE: the
`level091` that sits in the MAIN tree is not the build the picks came from (e63 s17 decodes to a different
mesh there); rebuild before decoding.

## Tools added

* `verify/geo_r1021b_void.py <level dir>`: rasterises every world triangle top-down (300 units/cell), keeps the
  cells within 30000 units of a race span and counts the connected components of UNCOVERED cells (sky/void
  from above). This is the "junction void" ledger.
* the 1015 B audit (`verify/geo_r1015b_audit.py --main`) for pavement over carriageway and street under ground.

## What was changed

| Item | Cause (measured) | Change | Knob |
|---|---|---|---|
| 7, 8 (texture) | `e36 s33` and `e35 s26` are Calle 16, OSM `surface=sett` (68 ways on the route): `tg_road_page_for_geo_surface` answered the cobble art (page 43), no lane paint | a cobble crossing takes the city's own marked street page | `TD5RE_GEO_XSTREET_ASPHALT=0` |
| 8 (pavement) | the street leaves the FAR kerb of Diagonal 73 (mouth shift > 0). Its flank corners sit on spans whose main pavement does not stand (the corridor owns that edge), so `tg_r11_arm_side` returned 0 on both neighbours and no pavement arm was ever laid | far-kerb mouths lay both arms unless the neighbour span is itself a mouth | `TD5RE_GEO_FAR_STREET_ARMS=0` |
| 5 | the main road's kerb slab is dropped only on the spans the mouth table lists; a street leaving at 45 degrees sweeps one span further along the road at the slab's lateral, so the next span's slab stood on the tarmac (`city over city:101`, 97 of 124 overlap samples) | the slab is CLIPPED to its free run, cut along the street's own edge, with a cap on the cut (the 1015 A far-footway clip, now taking every mouth, not only far-kerb ones) | `TD5RE_GEO_PAVE_STREET_CLIP=0` |
| 2 | at the first span of the fork corridor the far footway steps from lateral -4960 / 2467 wide (span 40) to -3000 / 1290 wide (span 41), and the width alternates 1290 / 2600 span by span along the run. `tg_av_far_joins` refuses a join over 60 units, so every boundary got a vertical cap | width / lateral differences under 400 units are blended (both spans take the mean at the shared node, no cap); a real lateral step and the end of a run become a RAMP (the slab top falls to road level over that span: same footprint, so no hole). Street mouths keep their clipped cap | `TD5RE_GEO_FARWALK_BLEND=0` |
| 17 | `tg_net_march` stops a street where its centre comes within half-width + 200 of another street's paint, leaving a bare gap of up to 3000 units (7 m) in front of the flank wall (span 455 right, span 202 left) | a street that was stopped by a street is lengthened to touch it (only if it already cleared the minimum alone, capped at the flat verge) | `TD5RE_GEO_STREET_JOIN=0` |

## Not fixed

* **10 (junction with no geometry after e63 s17).** Measured with the new void audit: ONE real void on the
  route, 203 m2 at span 256 (plus the two route ends). It lies south of the road over spans 256 and 257, in the
  tail of real fork 0 (Diagonal 73, F=40, ring window nodes 26..271): `e63 s14` (span 255) has its south skirt
  slab, the next two spans (`e64`, spans 256 and 257) have NO ground mesh at all (their slots hold only the
  road), and span 258 on is back. The corridor road ends at span 255 and the avenue at 255, while the window
  runs on to 271. Not touched: it is the fork-merge territory the avenues/forks child owns, and I could not
  find which cull empties the skirt without an instrumented build. No other junction void exists on this route
  (every other uncovered component is under 25 m2). The "multi-way / skewed junction" fill is therefore not a
  void problem on this route but a coverage one (items 15 and 17).
* **15 (road that joins the main road not rendered, `e85 s6`).** The real way is Calle 54 (OSM ways 510 /
  717 / 466, `primary`, 2 lanes): it starts 18 m beside the route at span ~340, runs along the road and merges
  into it at span 368 (way 499 in the reader's index, junction (516479,-24608)). `NET_DIAG` shows no arm at all
  near span 343 to 360 (it is a frontage road, not a crossing, and the shared-carriageway rule only registers
  the vertex where it joins) and the one arm that exists, at span 368, is refused `short` (3922 units against
  the 4200 minimum `TD5_TG_R8_CLAMP_MIN`). Rendering it means a new kind of edge (a road that runs parallel to
  the route at 2 to 8 m from the kerb); not attempted.
* `branchroad over city:101`: 17 samples remain after the clip (the 16-sample free-run search leaves a sliver
  where the cut is oblique).

## Numbers (offline audit, same route)

Rebuilt with `verify/geo_r1015b_build.ps1` (parent exe `td5re_master.exe` vs this branch), audited with
`verify/geo_r1015b_audit.py --main` and `verify/geo_r1021b_void.py --min 8 --cell 300`:

| | master | this branch |
|---|---|---|
| pavement samples inside a carriageway | 124 / 31328 (50 meshes) | 61 / 30272 (24 meshes) |
| `city over city:101` (main kerb slab on a street) | 97 | 17 |
| `branchroad over city:101` (the clipped slab's residual) | 0 | 17 |
| street surfaces with ground / pavement above them | 83 of 678 meshes (1436 samples) | 63 of 689 (1299) |
| junction voids >= 8 cells (300 units), excluding the two route ends | 5 (203 m2 at span 256 + 4 under 25 m2) | 4 (203 m2 at span 256 + 3 under 25 m2) |
| streets joined to the street that stopped them | - | 2 (spans 202 left, 455 right) |
| far-footway spans ramped / blended instead of capped | - | 271 |

Race census (`[NET/GEO] real street census`): 157 arms considered, 52 accepted (street 39, avenue 13), dropped
skew 6 short 88 taken 11, march stops street 9: identical before and after. `[REAL FORK]`: 2 driveable forks,
420 corridor spans, identical. STRIP.DAT (81750 B, `2AF7CC7D...`) and TEXTURES.DAT (1605328 B, `5B4E005A...`)
are byte-identical to master's, so no collision, AI or texture-page data moved: only MODELS.DAT changes
(8398608 B -> 8388032 B), which is why `geo_fork_validate` was not re-run.

## Gates

* synthetic slot 60 (seed 99991, `SELECTED.TXT` moved aside): MODELS.DAT 12772392 B `E2F1F33C221D61CAB0EFF701484011F0`,
  STRIP `0641EDB7...`, TEXTURES `5B4E005A...`: identical to master's exe.
* knobs-off on the geo route (`TD5RE_GEO_XSTREET_ASPHALT=0 _STREET_JOIN=0 _FARWALK_BLEND=0 _PAVE_STREET_CLIP=0
  _FAR_STREET_ARMS=0`): MODELS.DAT 8398608 B `8965ACC965B8FEA1FE0C0F85D0D06981` = master's exe. Knobs on:
  8388032 B `85AF13869630D036E95ADEE90327CA74`.
* `build_all.bat`: dev + release OK, structure lint OK (extern 3/3, game.h includers 23/23, warnings 84/84).
