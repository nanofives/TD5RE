# Autotrack feedback round 7 (2026-08-30)

Verbatim user feedback from a manual drive of the merged round-6 build
(`28d70652`, autotrack seed **99991**, track slot 60). Rounds 1-6
(30+25+19+20+18+19) are merged; this is round 7, **19 items**.

Seed 99991: CITY 0-449 and 600-899; forks 144-169 / 319-360 / 510-631;
corridors 1801-1824 / 1826-1865 / 1867-1986; tunnels 480-499 / 1520-1539;
bridge runs 1000-1039 / 1160-1199 / 1320-1359 / 1640-1679.

## A. Geometry ON the road (see the triage -- treat as ONE systemic defect)

5. On span 192 there's a building that rendered over the road.
11. On span 598 there's a texture of background city texture that should ONLY be
    used when it's far away behind buildings, and here it's crossing the road
    overlapping with road.
13. There's overlapped geometry over the branch road on span 626.
15. There's plenty of buildings in the middle of the road on span 651. **You
    need to add an additional way to check for things being rendered on the
    road.**
19. There's grass rendered over the road on span 1415.

## B. Crossings

1. Crossings are much better, but the sidewalks are not generated on the side of
   the perpendicular streets.
2. Make the perpendicular streets longer so it looks more immersive.
14. On the curve of span 635 there are many crossings all together; this
    shouldn't happen.

## C. City

3. On span 137 the plaza of tiled flooring looks fine, but the buildings at the
   back look like they don't have ground or don't reach the floor fully. The
   plazas can contain other elements like fountains and people, and sidewalk on
   the side of the road.
4. We need to incorporate more building variety to the pre-selection of
   buildings for the city biome.
7. Transitions like on span 455 should keep continuity on elements like sidewalk
   height and guardrails until the very end of the biome.

## D. Branches

6. Before the branch starts on 318, the right side of the sidewalk is not
   rendered and it looks like it has a gap, see-through.
10. At the side of the branch on 569 there's barely any grass to the side and
    nothing else. You have to add more scenery on the side if it is grass.
12. Around span 611 the transition of the median is grass and tiles; it should
    not switch between them.

## E. Bridges / tunnels

9. The entrance of the tunnel on span 477 has a weird gray texture that I don't
   know if it's poorly decoded or skipped altogether. It doesn't look like the
   entrance of a tunnel. **You should investigate the tunnel entrances of other
   TD5 tracks.**
16. The pillars on the bridge on span 1000 still are finishing on the guardrail
    and not touching the ground. The texture of the pillar doesn't seem fine.
17. I can still see on span 1023 below the bridge a mix between tiles at one
    height and water on alternated spans at a lower height. Below bridges only
    water should be rendered.

## F. Flora

8. On the right on span 462 you are using a tree texture of something that
   shouldn't be a billboard. It's an orange-ish tree that looks like the texture
   is cut off because it can be continuous.
18. On the left on span 1120 there's water near the road but trees floating in
    the air. Add a check to see if trees are touching ground, and if it's water
    or a coastline trees should not be placed.

---

## Triage notes (orchestrator, 2026-08-30)

### 1. FIVE ITEMS ARE ONE SYSTEMIC DEFECT, AND THE USER HAS NAMED THE FIX

Items 5, 11, 13, 15 and 19 are all "something is standing in the road": a
building, a background city texture, unnamed branch geometry, more buildings,
and grass. Different emitters, different biomes, same failure.

**This is the FOURTH consecutive round with on-road geometry**, despite
`tg_carriageway_reach` / `tg_on_carriageway` / `tg_carriageway_clear_gap`
existing precisely to prevent it. Rounds 3, 4, 5 and 6 each fixed *individual*
emitters that forgot to ask the authority. That approach has now failed four
times, because it relies on every current and future emitter remembering.

The user's item 15 states the correct answer outright: **"you need to add an
additional way to check for things being rendered on the road."** So round 7
gets a dedicated area whose job is a GLOBAL BACKSTOP, not another per-emitter
patch: a post-emit validation pass that tests emitted geometry against the
drivable surface and rejects (or at minimum loudly reports) anything overlapping
it. Done once, it catches all five of these AND every future one, and it turns
"scenery on the road" from a recurring bug class into a build-time assertion.

Note the existing `geometry-safety` self-check reports "clean" on this seed
while five separate objects sit in the road — because (established in round 5)
it only tests centreline self-overlap and fork reach, not scenery containment.
Extending that check is the natural home for this.

### 2. FOUR FRAME-VERIFIED ROUND-6 ITEMS CAME BACK. REFINE THE METHOD.

Round 6's rule ("byte attribution is not an acceptance test; get a frame of the
complaint") was right and did work -- but it is not sufficient on its own:

- **item 16** (bridge piers) -- R6 BRIDGE frame-verified `12_piers_BEFORE/AFTER`
  and the user says they still stop at the guardrail rather than the ground.
- **item 17** (tiles under the bridge) -- R6 BRIDGE frame-verified continuous
  water; the user still sees tiles alternating with water at a lower height.
- **item 8** (span 462 tree) -- R6 FLORA frame-verified at 462 and fixed a grove
  backdrop being planted upright. The user now reports a DIFFERENT tree at the
  same span (orange-ish, cut off), i.e. one page in the rotation was fixed and
  another has the same defect.
- **item 9** (tunnel entrance) -- R6 TUNNEL frame-verified a new portal page;
  the user reads the result as "poorly decoded or skipped altogether".

**The refinement: a frame proves ONE instance at ONE span. It does not prove the
CLASS.** For round 7, an area must additionally show the fix holds across the
other instances of the same object (other bridge runs, other pages in a
rotation, the other tunnel mouth). Where the complaint is a whole category
("tiles under bridges", "trees as billboards"), the acceptance test is the
category, not the photographed instance.

Item 9 adds a second lesson: "poorly decoded" is a real hypothesis. Verify the
page CONTENT is what you think (dump it) rather than assuming the id you wrote
produced the art you intended. The user's suggestion to study how the shipped
TD5 tracks build tunnel entrances is good and cheap -- that art exists in
`re/assets/`.

### 3. Two items are asset/design breadth, not defects

- **Item 4** (more building variety for the city pre-selection) and **item 3**'s
  plaza dressing (fountains, people, kerb) are content-expansion work, the same
  shape as round 5's tree-library item. The extractor built then
  (`gen_trackgen_r5flora_tex.py`) and the confirmed finding that shipped tracks
  hold unmined art both apply here.
- **Item 2** (longer perpendicular streets) changes generated shape -- watch for
  "boxed in" and confirm races still finish.

### 4. Smaller notes

- **Item 18** is a placement-validity rule with a clear statement: a tree must
  rest on ground, and must not be placed over water or a coastline. Same family
  as the road guard (item 15) -- both are "validate what you emitted".
- **Item 12** (median alternating grass/tiles) is a per-span material roll where
  a per-RUN choice is wanted; compare with how the divider treatments are chosen
  per fork ordinal.
- **Item 7** (continuity to the end of a biome) and **item 6** (see-through gap
  before a branch) are both boundary-condition bugs at a run's final spans --
  worth checking whether one off-by-one covers both.
