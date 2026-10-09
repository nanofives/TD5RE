# GEO STREETS AND PLAZAS (round 1014 group B)

La Plata, level091, picks 6, 8, 11, 14, 19. All changes are geo-only and gated: the
synthetic slot-60 track is byte-identical (MODELS.DAT `298DB07B141160AAFED83C3941DD1580`,
12 982 584 B, STRIP 144 714 B, TEXTURES `F69A8CBB...`, parent exe and this branch).

## Method

`verify/geo_r1014b_frames.ps1` (chase-cam framedumps, one launch per exe, any
`TD5RE_*` knob through `-Env`) plus a top-down plot of the generated level
(MODELS.DAT meshes at (entry, slot) coloured by page, NETWORK.JSON edges, ROADS.JSON
ways), so every claim below was read off both the plot and an in-game frame before and
after. `TD5RE_GEO_NET_DIAG=1` logs every junction arm (way, span, side, skew, run) and
its outcome; the ledger the census prints is capped at 96 lines and was hiding the
cause of item 8.

## Item 6: a sidewalk on top of the crossing

`tg_r14_fork_nostreet` (R14) kept the main-road slab and kerb railing across every
mouth inside a fork window, on the grounds that the synthetic `tg_r10_cross_gates`
refuses side streets there. On a geo track the street is real and is laid on the side
the corridor does not take, so the slab covered the mouth (span 61 and 62 left: a
3.5 m slab plus a railing). `tg_r14_fork_nostreet_s(si, left)`: on a geo network
`!tg_facade_built` is exactly "the network opened a street here", so the exception
does not apply. `TD5RE_GEO_FORK_MOUTH_PAVE=0` restores it.

## Item 8: the street on the other side of the avenue

Every mouth started at the RACE kerb. A real street crossing Diagonal 73 is split by
OSM at both carriageways, so its far half is a T on the OTHER carriageway. The arm was
found (`way 139 right si 114 run-from-kerb 539150`) and then refused as `corridor`
(`tg_side_corridor_here`: the real fork window covers every opening by design, and the
avenue's reach counts as a corridor) or, for the 14 m connector, as `short`.

Fix: the mouth table carries a per-span origin shift (carriageway reach minus the race
half width). `tg_city_edge_frame` returns the shifted kerb for those spans, which is
what the street quad, its pavement arms, flanks, zebra and reveal row all read, so they
move together; the reveal row and the prop envelope add the shift explicitly. The mouth
span is the one nearest where the street's OWN centre line meets the far kerb (a street
at 45 degrees reaches it a carriageway further along the road than where it was found:
without this the far half was 2300 units off its line). The far footway breaks at the
mouth. 25 then 22 streets start beyond an avenue (the count moves with the width rule
below). `TD5RE_GEO_FAR_STREETS=0` restores the old placement.

## Items 11, 14, 19: widths and the roads round a plaza

* **Width.** OSM counts a plaza ring's `lanes` as 2 (7 m) while a calle has had the
  place's 10.47 m since 1011, and the roads reader that draws the cross streets never
  applied the table (every cross street 7 m). `NAMEK_PLAZA` gets the carriageway in the
  route reader, the roads reader and, at generation, on the route's own plaza spans
  (`tg_geo_plaza_floor`: an existing `_route/` and a fresh BUILD agree without anyone
  rebuilding). A street leaving at `skew` is only cos(skew) as wide as the frontage run
  that draws it (0.71 at La Plata's 45 degree grid, 0.37 at the Plaza Moreno mouths):
  the run is widened by 1/cos up to 55 degrees.
* **A short wide blip.** Plaza Moreno spans 588..591 are lanes 6 between 2 and 3 (an
  undivided Diagonal 73 way the 1012 D2 rule counts as two carriageways), which the one-lane-per-seam
  ramp turned into a six-lane diamond with the markings of six lanes converging and every
  street arm leaning back across it. A run of at most 8 nodes two or more lanes wider than
  BOTH ends is capped one lane over the wider end (4 nodes on this route, 2 to 6 lanes becomes
  2 to 4). `TD5RE_GEO_LANE_SPIKE=0`. The profile (plaza floor + blip cap) is one cache read by
  the walk and the real-fork planner so they cannot disagree.
* **Roads that border a plaza.** The minimap draws every way; the world drew the route
  and a 45 m straight stub per junction arm, so a road that curves round a square ended
  in bare ground (two thirds of Plaza Miguel de Azcuenaga's ring, and the arcs round
  Plaza Moreno). `tg_net_geo_plaza_roads`: for every plaza bound to the route (17 on
  this route), the real ways that RUN ALONG its edge (a sample within 9000 units of the
  edge and within 45 degrees of its tangent, so a street that merely crosses it does not
  qualify) and are not already road become backstreet-kind network edges, emitted as ONE
  mitred ribbon with a raised footway each side. Painted after the pass, so a way's
  own joints are not refused by the previous ribbon's paint. 98 ribbons over 65 ways.
  Scenery, not a fork.

## Not done, with evidence

* The plaza fork (a drivable ring) is still the free-geometry corridor design in
  `GEO_REAL_FORKS.md`; the ribbons are not drivable.
* Streets steeper than 55 degrees are still the old thin wedge (a sheared quad cannot
  be a wider street) and their pavement arms lie across the carriageway; lowering
  TD5RE_GEO_NET_SKEW_MAX_DEG to 62 was tried and is worse (another arm took the slot as a
  20 m triangle), and ribbons where two plaza ways overlap (Calle 50 and the Calle
  14 arc at Moreno) z-fight over a few metres.
* Ribbon junctions with the route leave a 1 to 2 m gap (the ribbon stops outside the
  route's paint).

## Measurements

* Census (arms accepted, same route): 58 before, 64 after; 22 start beyond an avenue.
  Right-side streets along Diagonal 73 (spans 24..253): 0 before, 7 after (60, 114, 116, 169/172, 226, 228).
* Synthetic slot 60: MODELS.DAT 12982584 B `298DB07B141160AAFED83C3941DD1580`, STRIP 144714 B,
  TEXTURES `F69A8CBB6A3FFCA4757360F5AC39234B`: identical on the parent exe and on this branch
  (SELECTED.TXT moved aside).
* All-AI race, 6 cars, 4x (`verify/geo_realfork_run.ps1`, report `geo_realfork_report.py`): every car
  reaches the finish area before and after. Stalls / contact events / incidents: 7 / 327 / 75 before,
  8 / 303 / 68 after (route 229..355 around the plaza ring 91 -> 16 events). Plateaus at 471..473
  and 599..601 are stalls of two cars for about 2 s each. 471..473 (fork 3's ring spans, mouths
  465 L, 477 R) shows in both after runs and not in the before run; contact INCIDENTS in that
  corridor are equal (6 and 6) but the bounce count is 89 against 15, so one car sits against a
  wall there longer. Not traced to a cause; the corridor geometry itself is untouched, the streets
  beside it are wider. Worth a look in the next AI round.
* Structure lint: warnings 84 -> 83, no new extern, no new game.h includers.
