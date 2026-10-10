# GEO STREETS AND PAVEMENTS (round 1015 group B)

La Plata, level091, picks 2, 5, 7, 12, 16. All changes are geo-only (gated on a loaded place) and
each has a knob that restores the old behaviour. Synthetic slot 60 is byte-identical on this branch:
MODELS.DAT `298DB07B141160AAFED83C3941DD1580` (12 982 584 B), STRIP 144 714 B `0641EDB7...`,
TEXTURES `F69A8CBB...`, with `re/assets/geo/SELECTED.TXT` moved aside. On La Plata STRIP.DAT and
TEXTURES.DAT are byte-identical to the parent exe (`a6b080e4...`, `70b42907...`): nothing here moves
a collision surface, MODELS.DAT grows 15 696 748 -> 16 186 288 B (+3.1 %, the span-length street pieces).

## Method

`verify/geo_r1015b_audit.py <level dir>` reads MODELS.DAT + MESHTAG.BIN only and counts the two
things the round asked for:

* **overlap**: a sample of a PAVEMENT triangle (page 44) that is inside a carriageway triangle in
  plan view, itself and four neighbours 200 units out, with the pavement within -300..+1500 units of
  the road surface. Carriageway = road / branch-road page 0 and the cross-street page 101. The R15
  median island (a 3-quad raised strip down the middle of a wide street, 520 wide, by design) is
  counted separately and not as an overlap.
* **under**: samples of a STREET surface (page 101 on city/cross kinds, branch-road page 0, optionally
  main road page 0 with `--main`) with a ground (5, 2, 65) or pavement (44) triangle more than 40 units
  above it.

`verify/geo_r1015b_plot.py` is the top-down plot used to find causes (meshes by page over the real
OSM ways and NETWORK.JSON edges; painter's order, not a z-buffer, so read hidden-ness off the audit),
`verify/geo_r1015b_build.ps1` rebuilds La Plata with knobs and keeps the generated files and a clean
race.log (the window must be closed clean or the census does not flush; it waits 40 s after
MODELS.DAT settles because a WM_CLOSE sent mid level-load is not pumped), and
`verify/geo_r1015b_tour.ps1` is the 1014 A free-cam tour with `-Offset` (see "Tour gotchas").

## Item 2: buildings along a road that is not drawn

Picks decode (MODELS.DAT + MESHTAG.BIN): `e11 s13` is a `city` house block (page 68+8) 31 m from
the route at span 43. Real OSM streets there: ways 136 / 2193 (span 37) and 1220 / 2192 (span 41).
`TD5RE_GEO_NET_DIAG=1` shows all four arms found and then refused `grid`: `tg_geo_span_run_ok` rejects
any mouth whose run starts below `TD5_TG_FACADE_START_RUN` (60) while START IN TOWN is on, and
`tg_facade_built` forces the frontage built there. On a synthetic track that courtesy keeps the start
grid city-like; on a real place it deleted the two streets and left the OSM buildings (which are placed
from the footprints, not from the frontage rule) standing beside nothing.
`tg_start_city_run()` returns the start grid (`TD5_TG_GRID_SPAN + 2` = 26) on a real place and 60
otherwise; both authorities read it. `TD5RE_GEO_START_STREETS=0` restores 60.

## Item 7 (and the street half of 2 and 5): the street under the tiles

`tg_city_emit_crossstreet` laid one rectangle per span, kerb to reach (19 500 units), at kerb height
falling by at most `TD5_TG_GROUND_DROP`. A diagonal street stays beside a CLIMBING avenue, so the main
road's later spans (skirt = road level - 70 out to the conformed bed) and the far terrain rose over its
far half. Audited before: 151 of 1063 street meshes had ground or pavement above them (median 124
units, max 1205); `city` streets 107 of 164 meshes. The picked mesh `e24 s29` was covered in 5 of 8
samples by `skirt:5` and `terrain:5`.

`tg_xstreet_push_follow` cuts the quad into span-length pieces and lifts every vertex past the kerb to
the heightfield envelope (max over half a cell round it) + 60 where that is higher than the old ramp.
The heightfield carries the road-bed conform, so beside the avenue that is road level and out in the
blocks it is the natural ground. The kerb edge is unchanged, so the mouth and the arms still meet it.
The ring-road ribbons and back streets (`tg_net_emit_entry`) get the same treatment.
`TD5RE_GEO_STREET_FOLLOW=0` restores both.

Result: `e24 s29`, 5 of 8 samples under -> 0 of 104. `under_street` meshes 151 -> 118 (city kind
107 -> 82 of 162). Frames: item 7 top-down before = tiles where the street should be, after = tarmac
with lane markings; item 2 span 41 oblique before = a grey tile gap between two rows of buildings,
after = a four-lane street with markings, arms and the buildings on both sides.

## Item 16: pavement through the middle of the road

Picked `e200 s43` is a 4-vertex arm slab of a street at span 805. The road swings 90 degrees over spans
801..805; the mouth skew was measured at the placing span and copied to every span of the run, and
every span's quad, arms, flanks and zebra rotate THEIR OWN outward normal by it. In a bend the
normals turn, so the five quads fanned: the ones at the start of the bend pointed back across the
carriageway and their arms ran 20 m over the road. `under_main` (main road with pavement above it):
17 meshes (115 samples) before.

After a street is placed, each mouth span gets the skew that turns its normal onto the validated ray,
and a span whose normal is more than the skew ceiling (65 degrees) off it leaves the run
(`mouth spans cut off a bend's fan 2` on the census line). `under_main` 17 -> 1 meshes (115 -> 2
samples); `block over road:0` 15 -> 6 samples. `TD5RE_GEO_MOUTH_PARALLEL=0` restores the shared skew.

## Item 12: sidewalks on the road round the plaza

The 1014 B ring ribbons lay their footways as if each ribbon were alone. Where two ring ways run side by
side or cross (the Y at Plaza Moreno, Calle 50 against the Calle 14 arc) the inner footway of one lay over
the other's tarmac: 389 audited samples of `cross` pavement over `cross` carriageway. A footway section
whose centre lies on another edge's carriageway is now dropped (`tg_net_on_other_road`): 389 -> 26
samples. `TD5RE_GEO_PLAZA_FW_CLEAR=0` restores.

The picked mesh itself, `e59 s35` (`city`, 7 quads at span 239..240 beside Plaza Azcuenaga), is the main
road's own right pavement ending square at the mouth of a 45-degree street; it does not overlap a
carriageway in the audit, so what Mariano sees there is the square end against the oblique street edge
(item 5's mechanism, below), not a slab on tarmac.

## Item 5: not fixed (the street it meets is now visible)

Picked `e22 s15` is `branchroad` page 44 + 121, the far footway beside the avenue's far carriageway
(round 1014 A, `tg_av_emit_far_pavement`). The street leaves the far kerb at 45 degrees, so at the
footway's lateral its edge has swept up to one span further along the road than the mouth table says; the
span next to the mouth still carries a slab over the street's oblique edge (2..8 audited samples per
mesh, 40 samples `branchroad over city:101` in all) and the slab ends square against an oblique edge.

I wrote the check that drops a span's slab when a mouth quad covers any of nine samples of it
(`docs/plans/GEO_STREETS_1015B_far_footway_clip.patch`, 89 lines, knob `TD5RE_GEO_FAR_STREET_CLIP`).
It took `branchroad over city:101` 40 -> 0 and total overlap 221 -> 181, but dropping the whole span
left a 1500-unit gap with a 1 m hole behind it (the slab was the only cover there), visible in the
top-down frame as a teal sliver. It is not shipped. The right fix is to CLIP the slab to the free
`u` range of the span (one slab, interpolated nodes, a cap at the cut) inside
`tg_av_emit_far_pavement`, which is round 1014 A's function and was being edited by group A at the same
time, so it is left for the integration.

## Not done, with evidence

* 82 of 162 `city` street meshes still have something above part of them (2 749 samples): pavement slabs
  (`city:44` 620 samples, median 92 above), verge bands (`city:5` 339, median 70), park lawn (`block:65`
  130). These are the mouth's own arm / verge band overlapping the street's first metres, not the ground
  covering it; the picked streets are clear.
* 29 of 53 ring / back-street (`cross`) meshes are still under the far-band terrain apron, nearly all more
  than 100 m from the route (median 188 above): the apron is interpolated between five profile stations
  along the main road's rays and is not the heightfield away from them, so `world_h` cannot be used to
  stay above it. The right fix is to cut the apron round a street or to stand the street on the apron's own
  profile.
* Census (same route, parent exe -> branch): 69 -> 71 arms accepted (street 61 -> 63), `grid` drops
  5 -> 0, 23 streets beyond an avenue (unchanged), 0 corridor drops, 3 real forks (unchanged:
  `[REAL FORK] 3 driveable fork(s) ... 409 corridor span(s)`).

## Tour gotchas (for the next person)

* A free-cam pose far from the start needs `--StartSpanOffset` AND the streamed scenery to have reached
  that entry: the worker decorates entries from the start in order, so a car parked at span 800 draws the
  untextured ribbon until entry ~200 is done (about 25 s). `-Env "TD5RE_FREECAM_TOUR_SPAN=<off+3>|
  TD5RE_FREECAM_TOUR_FREEZE=1|TD5RE_FREECAM_TOUR_DWELL=4500"` parks the car, pauses the sim and waits.
* A fresh worktree's first launch writes a 640x480 td5re.ini: set Width/Height before reading frames.
