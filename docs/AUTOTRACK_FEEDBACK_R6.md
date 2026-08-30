# Autotrack feedback round 6 (2026-08-29)

Verbatim user feedback from a manual drive of the merged round-5 build
(`cbb541ba`, autotrack seed **99991**, track slot 60). Rounds 1-5
(30 + 25 + 19 + 20 + 18 items) are merged; this is round 6, **19 items**.

Seed 99991 reference: CITY spans 0-449 and 600-899; forks at 144-169
(avenue, sep 0.20), 319-360 (sep 0.59), 510-631 (sep 1.00, widest); corridors
appended 1801-1824 / 1826-1865 / 1867-1986; tunnels 480-499 and 1520-1539;
bridge runs 1000-1039, 1160-1199, 1320-1359, 1640-1679.

## A. Intersections

1. On city biome, street intersections still have the sidewalks lifted during
   the crossing. The sidewalk should STOP when there's a road intersection.
   There should also be sidewalks on the intersecting street. Also there's
   double geometry on the buildings on the side.
2. There's an intersection where instead of road I see green grass — fix this to
   be street. I don't want street crossings for grass. It still has that
   vertical green texture as guardrail.
15. Avoid adding crossing streets right after a bridge.
19. On zones with smaller buildings, make sure crossing-street buildings have
    the same height.

## B. City massing

3. On span 135 I still see the line of skyscrapers on the right of the track,
   which is looking pretty bad.
4. On the right track of the branch on span 150 I still see the background
   texture for buildings crossing the street; same on span 154.
6. On the zone around span 188 there's still geometry that overlaps with the
   road, rendering invisible what's forward. Severe, bad looking.
11. On span 900 there's a building cut off on the sides that lasts only 1 span.
    It doesn't have guardrails like the rest of the spans on the sidewalk, and
    it's not a crossing.
16. After the bridge on span 1043 there's 1 span of building that's not
    connected with anything. If this is the transition, remove the logic — it
    looks wrong and detached from its surroundings.

## C. Branches

5. Floor on the right side of the right track branch MOVES as the car is moving.
9. There's a small gap on span 519 on the right track branch and the sidewalk
   that's see-through.
10. Span 570 collision is LIFTING the car. You have to avoid this kind of curve
    in the future, and you have to detach the geometry of the left track to
    mimic the right track — they can be different as long as they end up joined
    at the end.

## D. Tunnels

8. The textures on the entrance of the tunnel on 479 look wrong. Tunnel texture
   is still not correct. There's geometry crossing the road while inside the
   tunnel. You should add lights to the side of the walls of the tunnel.

## E. Bridges

12. The pillars crossing the bridge span 1001 are not reaching the floor.
13. Guardrail texture on bridge span 1001 should not be used. Guardrails that
    don't have extrusion should have some sort of transparent background with a
    proper guardrail texture.
14. Below the bridge there's patches of tile textures on top of the water, and
    the water is not continuous either.
17. All bridges have pillars every few spans and they all look the same. I want
    more variety — find more things done in bridges across the tracks.

## F. Flora

7. On span 462 I see a billboard texture of a tree that looks like it should be
   a background tree line instead of a billboard.
18. There are trees crossing the track on span 1413. Some billboard trees are
    bigger than the area where they are supposed to be and they spill onto the
    road.

---

## Triage notes (orchestrator, 2026-08-29)

### THE PATTERN THAT MATTERS: numerically-verified fixes came back; frame-verified ones stuck.

Sort round 5's outcomes by HOW they were verified and the correlation is near
perfect. This is the single most useful thing in this document.

**Verified with an actual before/after FRAME — user did not re-report:**
- R5 CROSS item 4 (park hedge, framedump + green-mask) — not re-reported.
- R5 STRUCT item 1 (banner legs, cropped 7x A/B) — not re-reported.
- R5 STRUCT item 9 tunnel WALLS (same-span A/B) — the swept bore is not
  re-reported; only the texture and a separate intrusion are.
- R5 BRIDGE item 14 far-band (top-down A/B frame) — the draped terrain is gone;
  what remains (item 14 here) is a DIFFERENT residue (tiles on the water).

**Verified only by bytes/inventory/"by construction" — ALL came back:**
- R4 BRANCH items 7/11 (texture swim), explicitly "fixed by construction, NOT
  visually A/B'd" -> **round 6 item 5, the floor still moves.**
- R5 CITY item 15 (disconnected building front), "not cleanly isolated
  visually", byte-verified only -> **round 6 items 11 and 16, still there.**
- R5 CITY items 6/7 (backdrop follows bowed branch), "visual not isolated"
  -> **round 6 item 4, background still crossing the street at 150/154.**
- R5 CITY item 5 (skyscraper scale), frame A/B at span 145 but the user's
  complaint was span 137 -> **round 6 item 3, still bad at 135.** Right fix,
  wrong span photographed.

**CONCLUSION, and it is now a rule for this project: byte attribution proves an
emitter CHANGED, never that the RESULT IS RIGHT.** It is a good pre-filter and a
terrible acceptance test. An area that cannot produce a before/after frame of
the actual complaint has NOT finished the item, and should say so loudly rather
than bank the byte delta as success. R5 BRIDGE solved the "I can't see it"
problem with `TD5RE_CAM_TOPDOWN` on a parked AutoRace — **that technique is
available to every area now and removes the usual excuse.**

### Items that are confirmations of things WE already found and did not fix

- **Item 8's "geometry crossing the road while inside the tunnel" is exactly
  what R5 STRUCT discovered and deliberately left alone.** It A/B-proved that
  far-band / skyline billboards intrude into the bore as grey slabs standing in
  the road (they vanish with `TERRAIN_FAR=0`) and did not fix it only because a
  sibling was editing that emitter. Its recommendation stands: **gate far-band
  emission for tunnel span-groups**, the same shape as R5 BRIDGE's fix which
  gated far-bands over bridge runs. This is a known cause with a known fix.
- **Item 6 (span 188) is R5 item 8, which CITY could not reproduce** and
  honestly left unfixed. The user has now added the decisive detail:
  it "renders invisible what's forward". That is an OCCLUSION symptom — a large
  near-camera surface, or a mesh with wrong winding/depth, not merely something
  parked beside the road. Treat as a render/geometry bug, not a placement one.

### Item 10 needs care: the symptom CHANGED after our fix

Round 5 fixed the span-570 escape (car flung off-map). The user now reports the
car is **LIFTED** at the same place. **Consider that our own rescue may be doing
it:** `geo_rescue_offmap` calls `td5_physics_wall_response` + `rebuild_pose`
every tick while the chassis is >1200 from all road. If it fires repeatedly at
that hairpin it could push the car upward. `TD5RE_OOB_DIAG=1` is enabled in the
user's build, so the log will show whether rescues fire at 570 during a lift.
**Check that FIRST before treating it as a fresh collision bug.** If the rescue
is the cause, the fix is to constrain the push to the horizontal plane.

The second half of item 10 is a DESIGN request, not a bug: the two carriageways
of a fork should be allowed to diverge independently ("detach the geometry of
the left track to mimic the right track... as long as they end up joined at the
end"), and hairpins that tight should not be generated at all. That is a
curve-safety / fork-shape change in the generator.

### Cross-cutting

- **Item 1's "double geometry on the buildings" and item 19's height mismatch
  are both about intersections not being treated as a unit.** R5 CROSS fixed
  the facade comb by emitting only at gap corners; a doubled wall at the corner
  suggests both sides now emit at the same corner. Check for a two-sided emit
  at the boundary span.
- **Items 12, 13, 14 and 17 are all one area's bridge furniture** and 14 is a
  residue of a fix that partially worked. Item 13 states a general rule worth
  applying beyond bridges: a guardrail without extrusion needs an alpha-keyed
  texture, not an opaque panel.
- **Items 7 and 18 are the same class**: billboard trees used where a treeline
  belongs, and billboards scaled larger than their footprint so they spill onto
  the road. Likely one sizing/selection rule behind both.
