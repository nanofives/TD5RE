# Walker lane-count-transition secondary re-tests (DA-T2 D9/D3) — RE 2026-06-12

## Symptom
"Invisible wall" on London at the **Tower Bridge narrowing** (span 68 type-7, lanes
6→4 toward the tower; spans 69-75 = 4 lanes; span 76 type-4 widens back to 6).
Confirmed AUTHORED TD6 geometry (native strip has the same types/lanes — NOT a
converter artifact). The car's LEFT wheels (sub_lane edge) trip the lateral wall
where the visible road still looks full-width.

## Two compounding causes
1. **Collision↔visual width mismatch.** The STRIP narrows the collision (lanes 6→4,
   rail vertices define a 4-lane road) but the VISUAL mesh doesn't show a matching
   barrier at the exact collision-rail position → a wheel inside the visible road but
   outside the narrowed collision rail correctly trips `td5_track_resolve_wall_contacts`
   (left rail fires when probe `sub_lane < 1` AND `pen < 0`). This is the "invisible
   wall at widened/narrowed sections" class.
2. **Sub_lane carry not byte-exact across a lane-COUNT change** (documented
   td5_track.c:3871-3878). The port's `resolve_neighbor` cases **0x08 (LEFT)** and
   **0x04 (BACK)** do the span move + lane carry but OMIT the original's SECONDARY
   cross-product re-test that refines `sub_lane` by ±1 to the geometrically-containing
   lane at the new span. So the carried lane can land one off → wrong rail tested.

## The fix basis — original UpdateActorTrackPosition @ 0x004440F0 (FULLY decompiled)
The original computes a 4-bit boundary-cross mask `bVar8` (4 edge cross-products on the
CURRENT span's lane quad), then `switch(bVar8)`. Cases **1 (left)** and **4 (back)** —
the two the port's single-bit path under-implements — after stepping to the neighbor
span, RE-TEST the boundaries AT THE NEW SPAN and adjust the carried sub_lane:

- **case 1 (LEFT cross), after the step:** recompute edge mask `bVar10` at the new span
  (`<1 → LUT_X[type]`, `>=lane-1 → &LUT_29[type]`); then:
  - if `(bVar10 & 1)` and the LEFT-rail cross-product `>0` → commit, `sub_lane = uVar16-1`;
  - else if `(bVar10 & 8)` and that cross-product `>0` → follow into the junction/return
    (type 9 link_prev, type 0xb prev-lane test, else span-1), `sub_lane` adjusted by the
    height-nibble delta `-1`;
  - else → `sub_lane = uVar16-1` (no further move).
- **case 4 (BACK cross)** mirrors case 1 with `+1` sign and rails 2/8 instead of 1/4.

(Compound cases 2/3/6/8/9/0xC already do their own re-tests; the port ported those —
the residual is ONLY the single case-1 LEFT / case-4 BACK secondary re-tests = D9/D3.)

Key globals/LUTs (TD5_d3d.exe):
- `g_trackStripRecords` (24B/span), `g_trackVertexPool` (6B/vtx).
- Left-edge LUT `&g_trackContactNormalXLut[type*2]`, right-edge LUT `&DAT_00474e29[type*2]`.
- Vertex base offsets `&g_trackContactNormalYLut[type*2]` (li side) / `&DAT_00474e41[type*2]`
  (ri side) — these are the SAME per-type rail LUTs the port already mirrors as
  `k_rail_lut_l/r` in `td5_track_resolve_wall_contacts` and the case-cross helpers.
- sub_lane lives at `track_state[6]` (signed byte); span at `track_state[0]`;
  progress at `track_state[2]`, high-water at `track_state[3]`.

## Implementation plan (next session, careful + drive-tested)
1. In `resolve_neighbor` cases 0x08 / 0x04, after the span move + lane carry, add the
   secondary cross-product re-test (reuse the `k_rail_lut_*` + vertex-base math already
   in `td5_track_resolve_wall_contacts`) to refine `*sub_lane` by ±1 to the lane that
   geometrically contains the chassis at the NEW span. Native-faithful (it IS the
   original), so it can be UN-gated (not TD6-only) — but verify native tracks unchanged.
2. This alone won't fully resolve the bridge tower if the VISUAL mesh is genuinely wider
   than the strip — for that, either (a) widen the strip rails for the narrowed spans to
   match the visual, or (b) confirm/learn the visual mesh DOES narrow (tower base) and
   it's just hard to see. Check models.bin width at spans 69-75 vs the strip.
3. Verify: headless AI (no walker regression / desync / stuck on ALL tracks — native +
   TD6) + user drive-test the bridge + a native track (Newcastle) for no new jitter.

⚠️ This touches the CORE walker (every track). Do NOT rush it; headless can't see the
lateral feel — needs driving. See [project_td6_slope_junction_fallthrough_launch_2026-06-12]
for the related walker work + the desync DETECTOR (still in, watchdog).
