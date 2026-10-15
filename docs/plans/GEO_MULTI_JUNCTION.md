# GEO multi-road junctions (round 1030 F)

Feedback (Mariano, 2026-10-14):

- (13) "the crossing street level091 L91 e58 s0 skirt p5:GROUND pos 244717,1430,-247591 ... doesn't have road
  texture, this is a double crossing, it's only showing one crossing, maybe we need to rework the logic to accept
  complex intersections with multiple roads?"
- (18) "intersection around level091 L91 e83 s17 prop p0:ROAD pos 325579,-1194,-331763 looks wrong, it should be
  more similar to the beginning on the other side of the plaza"

## What the map has (level091, master 78a6b25f)

**Item 13, spans 224..230.** Diagonal 73 runs through a grid corner. Calle 43 (E-W) and Calle 20 (N-S) both cross
the route there, so each side of the route sees two streets leaving from one corner, fanning apart at about 90°
(a V).

| arm | way | side | window | fate on master |
|-----|-----|------|--------|----------------|
| Calle 43 east | 516 | right | 227..230 | placed |
| Calle 43 west | 533 | left  | 224..227 | placed |
| Calle 20 north | 606 | right | 224..227 | **TAKEN** (span 227 already Calle 43's) |
| Calle 20 south | 493 | left  | 227..230 | **SHORT** (run-from-kerb -390) |

**Item 18, spans 331..339.** This is the exit throat of the Plaza Miguel de Azcuénaga plaza fork (fork 1, F=230,
free rows k1=7..K2). AVENUES.JSON starts the second Diagonal 73 run at span 331. `tg_emit_geo_avenue` therefore
lays that avenue's far carriageway, island and paved infill beside the route. This is the orange slab and green
triangle in the pick. But the plaza corridor is still on its free arc there; its real far carriageway is the
corridor, which only rejoins at about 344. `tg_pf_scenery_clash` stands the scenery down only when a corridor row
is within 3 lanes, so these spans miss it. The same phantom avenue then pushes the real connector street
(Plaza Miguel de Azcuénaga, way 439, OSM 272846181) out past its kerb: far shift 5073, run-from-kerb 2290, SHORT.
At the entry (span 261) the matching connector E1 is a normal street with lawn either side.

## Rules

1. **Shared-corner (V) junction**, `TD5RE_GEO_MULTI_JUNCTION` (default 1). `tg_geo_span_run_ok` used to refuse
   an arm when any span of its window already had a mouth on that side. Now:
   - The arm is accepted on a trimmed window when **all** of these hold:
     - the taken spans are one run of up to 2 spans at one END of the window;
     - the remaining run is at least 2 spans;
     - the street that owns them leans the other way along the road (the two bodies fan apart from the shared
       kerb point, so they do not overlap).
   - The mouth table stays single-valued per (span, side). The two streets own adjacent spans, so every emitter
     that reads it (cross-street quads, pavement gaps, far-footway clip, frontage) works unchanged.
   - The march skips the "another street's paint" test for the first
     `(w_new/2 + w_old/2 + 200) / sin(angle between them)` units, capped at 15000. That is the stretch where the
     two bodies still touch at the corner. After it the normal test runs.
   - Any other overlap, such as two streets converging or running in parallel, is still dropped as TAKEN.
2. **Re-aim a kinked arm**, `TD5RE_GEO_ARM_REAIM` (default 1). This is tried only on an arm that would be dropped
   as SHORT. If its straight run broke at a vertex that sits within 10° of the hit segment's bearing, the arm is
   re-aimed along the chord from the junction to that vertex and measured again. The chord's points are all real
   OSM vertices, so no geometry is invented. Calle 20 south (2.8° kink after a 2265-unit first segment) is the
   case that needs it.
3. **Plaza corridor replaces the avenue's far carriageway**, `TD5RE_PF_AVENUE_YIELD` (default 1). Inside a built
   plaza fork's free window (rows k1..K2c), on the corridor's side, the side is the plaza and the corridor is that
   avenue's other carriageway. So:
   - `tg_emit_geo_avenue` draws nothing there except the existing plaza-walk footway;
   - `tg_geo_avenue_reach` answers 0 there. The skirt, the main-road sidewalk's avenue yield and the street
     far-shift all read that, so the route keeps its plaza-side kerb and pavement and the connector street starts
     at the route kerb, as E1 does at the entry.

Every rule is a no-op on builds with no geo place / no plaza fork, and knob 0 restores master's bytes.

## Not covered

- The crotch of a V is left as ground, with no corner pavement arm or block corner between the two streets.
- More than two streets on one side, and crossings that converge, are still refused.
- The mouth table remains one edge per (span, side); a true multi-valued table is a larger refactor touching every
  emitter that reads `tg_net_mouth`.
