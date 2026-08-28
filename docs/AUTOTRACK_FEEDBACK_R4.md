# Autotrack feedback round 4 (2026-08-28)

Verbatim user feedback from a manual drive of the merged round-3 build
(`a9584974`, autotrack seed **99991**). Rounds 1 (30 items), 2 (25 items) and
3 (19 items) are merged; this is round 4, **20 items**.

Drive conditions: dev build, RT HIGH (default), user driving (no AI override).
Spans quoted by the user (138, 570, 1163, 1169, 1178, 1339) are RAW strip spans
on this seed. Seed 99991 draws CITY on spans 0-449 and 600-899, tunnels at
480-499 / 1340-1359 / 1520-1539, bridge runs of 40 spans, and branch corridors
at 144-169 / 319-360 / 510-631 / 1801-1986.

## A. Banner / gantry

1. There's a vertical line at the center of the start banner (maybe finish too).
   The pillars for the banners don't look quite solid.

## B. Street crossings / intersections

2. Street crossings should have a break of the sidewalk which is a higher
   height.
4. There's a weird green texture on some crossings.
9. Street crossings should have lane markers on the perpendicular direction if
   it's a crossing.
10. Street crossings should go further and should have more buildings on their
    side.
14. Street crossings, if near to another on a curve, should be checked for being
    joined together for a sense of continuity.

## C. City massing / background

3. Background buildings on sidestreets look wrong, breaks immersion.
6. On span 138 I don't know the decision tree, but on the right side of the road
   where there's a lot of tiles there should be a park or buildings in the
   background depending on whether it's a park or not.
13. You can decide to make narrow roads of 2 lanes.
15. Some buildings still don't have sides, only a facade.

## D. Flora

5. Background trees look better but still pixelated and weird. Maybe we need to
   reclassify their height and which ones are suitable for cities. Also in the
   last race the texture has a black line at the top.

## E. Branches / avenues

7. Textures are moving on branches.
8. Dividers on branches have height, which is fine, but grass as walls is wrong.
11. Lane markers on branches are shifting all of a sudden and it looks weird
    when adding new lanes.

## F. Bridges / tunnels / water

16. On span 1163 you created a bridge with a sidewalk on the left, that's wrong.
    Guardrails have wrong texture (that's not a guardrail texture).
17. Around span 1169 there's a floating element.
18. Pillar on span 1178 looks wrong and has wrong texture.
19. On span 1339 there's a bridge going into a tunnel, that should be illegal,
    even moreso in a flat terrain.
20. Remember to add coastlines at the end of water perpendicular to bridges.

## G. Geometry safety

12. On span 570 there's an illegal curve that lets me go through the map and out
    of bounds.

---

## Triage notes (orchestrator, 2026-08-28)

### THREE root causes confirmed statically BEFORE any agent was spawned

These are read off the source, not guessed. Each owner starts from the cause,
not from the symptom.

- **Item 1 (vertical line at banner centre) is MY OWN round-3 fix.** The banner
  word does not fit one 64x64 page: shipped TD5 splits it over two consecutive
  pages laid side by side (`TD5_TG_PAGE_BANNER_L/R`). When I killed the mirrored
  back face at `ef206e41` I re-emitted the panel as **"Two half quads, one per
  page"** (`td5_trackgen.c` ~7462). Two independent quads meeting at the panel
  centre is exactly a seam down the middle. The word still needs two pages, so
  the fix is a shared edge / single strip across both pages, not two quads.
  The pillar half of the item is separate: legs are a 4-face prism at
  `TD5_TG_GANTRY_LEG_W` 220 x `TD5_TG_GANTRY_THICK` 160, and 160 raw is 0.625
  world units, the same too-thin dimension that let the back face win the depth
  fight. "Don't look solid" is a thin-prism problem.

- **Item 8 (grass as walls on branch dividers) is `tg_emit_avenue_divider`,
  and it very likely also resolves round 3's UNOWNED MEDIAN WALL.** That
  function (`td5_trackgen.c` ~5438) raises a 3-quad prism (top + two road-facing
  side walls) and textures **the whole prism with ONE page**: treatment 0 =
  `TD5_TG_PAGE_GREEN` at H=220, treatment 1 = `TD5_TG_PAGE_RAIL` at H=360,
  treatment 2 = `TD5_TG_PAGE_SIDEWALK` at H=150. So the planted treatment paints
  its two vertical walls with the grass page: verbatim "grass as walls is
  wrong". A planted median needs grass on the TOP quad and a kerb page on the
  two side walls. **Round 3 left a tall grey wall in the fork gore with no
  owner and two agents disagreeing; treatment 1 is a 360-high RAIL-paged prism
  in the gore. Check that first before re-opening the investigation.**

- **Item 19 (bridge running into a tunnel) is an unguarded independent roll.**
  The bridge run gate (`~1055`) and the tunnel run gate (`~4422`) are separate
  hash draws, each scaled by its own biome weight
  (`tg_biome_bridge_pct` / `tg_biome_tunnel_pct`), with **no mutual exclusion
  and no minimum separation between a bridge run and a tunnel run**. The
  inventory already shows tunnels at 1340-1359, so a 40-span bridge run ending
  near 1339 lands the deck straight into a bore. Needs an explicit interlock
  plus a clearance band, not a biome-weight tweak.

### Items that land on work previously reported done. Own that.

- **Item 15 (buildings still have no sides) was round 3 item 1**, owned by
  r3-city, which shipped `step-walls n=76`, all single-span runs. The inventory
  confirms the emitter FIRED, so this is not "did it run" but "does it cover
  every case". Single-span runs close a height STEP between neighbouring runs;
  a building at the END of a run, or an isolated-height building, may have no
  step to close and therefore still no side. Read the emit condition before
  touching the geometry.
- **Item 5 (background trees) is the THIRD round on tree lines.** Round 2
  fixed the flora band; round 3 established that the object actually on screen
  is the far-terrain ridge (`tg_emit_far_band`) and routed it to the green
  canopy page. The user now says "better but still pixelated", which is
  consistent with the right object finally being addressed and the remaining
  complaint being page RESOLUTION and biome suitability. The "black line at the
  top" is a new, specific defect: a band edge sampling outside the page, i.e. a
  UV that reaches exactly 0.0 or 1.0 without a half-texel inset.
- **Item 12 (out of bounds at span 570) is round 3 items 17 and 19.** r3-flow
  shipped map-edge walls, and `bind_boundary_sentinels` is DISABLED for custom
  tracks with the forward cap at 2 and reverse at ring-3. Span 570 sits inside
  the branch corridor 510-631, so the most likely gap is that the edge wall and
  the curve-safety clamp both key off the MAIN RING and neither covers corridor
  spans. `TD5_TG_CURVE_SAFETY` (1.5 * 1.2) is applied at resample time; confirm
  it is applied to branch node lists too.

### Cross-cutting notes

- Items 2, 9, 10, 14, 13, 20 and the park half of 6 are NEW GENERATION work and
  change track SHAPE. Same risk class as before: watch race.log for "boxed in"
  and confirm the track still completes.
- Items 1, 3, 4, 7, 8, 11, 15, 16, 17, 18 are correctness/appearance fixes on
  existing emitters.
- Items 7 and 11 (textures moving / lane markers shifting on branches) are
  probably ONE cause: a UV derived from a value that changes with branch width
  or lane count instead of from arc length. Whoever owns the branch area should
  test that hypothesis first rather than fixing two symptoms separately.
- Item 16's "sidewalk on the left of a bridge" is the same class as round 3
  item 9: an emitter not asking the carriageway authority whether it is over a
  bridge deck. `tg_on_carriageway` / `tg_carriageway_reach` exist for this.
