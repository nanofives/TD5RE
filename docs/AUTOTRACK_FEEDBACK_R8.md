# Autotrack feedback round 8 (2026-08-30)

Verbatim user feedback from manual drives of the merged round-7 build
(`25cfc7b4`). Rounds 1-7 (30+25+19+20+18+19+19) are merged; this is round 8,
**24 items** across **two seeds** plus five untagged design items.

Round 8 is the first round driven on **two seeds at once**. Span numbers mean
different places on each seed -- always state the seed with the span.

## Seeds under test

**Seed 99991** (the round 2-7 seed, track slot 60):
CITY 0-449 and 600-899, ORIENTAL, INDUSTRIAL, COAST.
Forks 144-169 / 319-360 / 510-631; corridors 1801-1986;
tunnels 480-499 / 1520-1539; bridges 1000-1039 / 1160-1199 / 1320-1359 /
1640-1679. The R7 on-road guard rejects 44 meshes here.

**Seed 777** (new this round):
INDUSTRIAL 0-149 · COAST 150-299 · FOREST 300-449 · ALPINE 450-599 ·
FIELDS 600-749 · INDUSTRIAL 750-899 · ORIENTAL 900-1199 · FIELDS 1200-1349.
No CITY. The guard rejects 0 here. **FOREST / ALPINE / FIELDS had never been
rendered before round 7** -- treat oddities there as first sightings, not
regressions.

---

## G. Untagged design / breadth items

G1. you should add more building variety, use different skyboxes for day and
    night, different start/finish banners, different guardrails
G2. you should add to the list of things to add lanes with slower lanes with
    different textures, driveable sidewalks like on scotland
G3. there can be major height differences like on scotland, san francisco or
    Newcastle
G4. keep incorporating all the elements from every TD5 track and if you don't
    know we can triage them, or highway like branches like the ones on moscow
    or maui
G5. you should add major branches that goes in completely different ways for
    longer and then come back like on sydney

## Seed 99991

1. crossings still need work, crossing specifically on span 75 the buildings are
   spawning on the edge of the sidewalks near the street, the street should be
   much longer (in this and all crossings)
2. on span 143 the sidewalk stop spawning here
3. the crossing on 187 has floating tiles on top of the road
4. the tunnel textures on span 477 still look weird, i don't want you to use
   this grey-ish texture on this place, the one with lighter gray stripes
5. on the right side of span 569 there's just a few meters of terrain outside
   road, it has to be a much bigger field
6. buildings look like they are all the same depth, some buildings should have
   more depth
7. there's tiles spilling over the road around 627
8. there's background texture over the road on 686
9. implement "continuation" of a perpendicular street, where you make a sharp
   turn, that turn could be coming from an existing street
10. pillars of bridges are still not touching the ground (they should go until
    the water
11. on the end of the bridge around 1031 there's alternation between water spans
    and tile spans at different height
12. avoid using city background texture when in the middle of a park

## Seed 777

13. at the beginning of bridge on span 161 if the bridge has sidewalk the
    guardrail should be on both sides
14. after the bridge on span 200 i can see the background texture for tree
    lines, which is fine but it looks stretched and very repeated, is the
    texture bigger than one span? should you use more texture variety?
15. on the same zone on the left side of the track there's barely any geometry
16. if there's snow the median should be snowy too but a different texture, and
    use different snow ground textures too
17. avoid green medians within tunnels like on span 562
18. you can make longer tunnels and bridges
19. if you include snow in the seed there has to be a prevalence for snowy
    themed biomes and cities to avoid major changes, you should include more
    biomes like beaches, different type of cities, add landmarks like on moscow
    or sidney, add one side sea like on sydney

---

## Triage notes (orchestrator, 2026-08-30)

### 1. THREE ON-ROAD ITEMS SURVIVED THE R7 GLOBAL GUARD -- AND THEY FIT ITS TWO KNOWN HOLES

Items 3 (`floating tiles on top of the road` @187), 7 (`tiles spilling over the
road` @627) and 8 (`background texture over the road` @686) are the same class
round 7 built `tg_guard_validate_entry` to end. The guard shipped, logs its
rejections, and drops 44 meshes on this very seed -- and three objects still
stand in the road.

The guard has exactly two designed escape hatches, and all three complaints fit
them:

- **The height gate.** The guard is height-gated so gantries, tunnel bores and
  bridge decks are not dropped for passing over the carriageway. "Floating tiles
  **on top of** the road" is, by the user's own wording, above road level. It
  passes the gate by construction.
- **The byte-range exemptions.** Road, gore, skirt, tunnel, bridge, water and
  gantry meshes are marked exempt at their emit sites. Zebra/crossing slabs are
  in that exempt set -- and item 3 is a *crossing* and item 7 is *tiles*.

So this is not "the guard failed"; it is "the guard's exemptions are too coarse".
The R8 GUARD area's job is to make the two hatches precise rather than to widen
the guard blindly: a height-gated mesh should still be rejected if it is *thin
and flat and near road level* (a tile is not a gantry), and an exempt mesh should
be exempt only for the span run it belongs to, not everywhere. Widening the
guard without that precision will start eating bridge decks -- R7 proved the
exemptions are load-bearing.

**Acceptance for GUARD is a class-level sweep, not three frames:** the guard's
own rejection log across every span of BOTH seeds, before and after, with the
count of newly-rejected meshes broken down by kind, plus proof that
road/deck/gantry/tunnel rejection counts stay at zero.

### 2. FOUR ITEMS ARE ON THEIR THIRD ROUND. THE BAR IS A REWRITE, NOT A PATCH.

- **Item 10** (bridge pillars not reaching the ground) -- R6 fixed
  "piers-to-floor" and frame-verified it; R7 re-verified at CLASS level across
  all four runs and both mouths and concluded the pier HEIGHT was already
  correct, attributing the perception to item 17's tiles. The user still sees it.
  R7's diagnosis was therefore wrong, or incomplete.
- **Item 11** (tiles alternating with water under the bridge end @1031) -- R6
  frame-verified continuous water; R7 submerged the gorge skirt below the water
  surface on a fast ramp. Still alternating.
- **Item 4** (tunnel texture @477) -- R6 built a new portal page; R7 built a real
  concrete portal facade projected outward past the buttress. The user is now
  specific about what is wrong: **not** "undecoded", but the wrong *choice* --
  "this grey-ish texture, the one with lighter gray stripes". That is a page
  SELECTION complaint, and it is actionable in a way the previous two rounds'
  readings were not.
- **Item 1** (perpendicular streets longer) -- R7 item 2 lengthened
  `tg_city_crossst_reach` from 1 to 2 backrow gaps (7100 to 10300). The user says
  "much longer", still, "in this and all crossings". One more increment is not
  the answer; the reach model itself needs to change.

**Ruling (user, this round): keep these in their natural areas (BRIDGE, CROSS)
but with a hard bar -- rewrite the emitter rather than patch it, and prove the
class across all four bridge runs, both tunnel mouths, and all crossings, on
BOTH seeds.** An area that reports "I adjusted a constant and here is a frame"
has not met the bar. Note also that R7's item-10 conclusion ("pier height is
correct") is a load-bearing prior claim that has now been contradicted by the
user -- re-measure it from scratch rather than inheriting it.

### 3. SEED 777's BIOMES ARE FIRST SIGHTINGS, NOT REGRESSIONS

Items 14, 15 and 16 land in FOREST / ALPINE / FIELDS, which no user had seen
before round 7. "Stretched and very repeated tree-line texture", "barely any
geometry on the left", and "no snow treatment for the median" are gaps in
coverage that was never built, not things that broke. Do not spend the round
bisecting for a regression that does not exist. The user's own question in item
14 -- "is the texture bigger than one span? should you use more texture
variety?" -- is a good hypothesis: check the tree-line band's UV period against
the span length before assuming the page is wrong.

Item 15 pairs with item 5 on the other seed (`just a few meters of terrain
outside road, it has to be a much bigger field`). Both are "the world ends too
close to the road". Same defect on two seeds, and R7's BRANCH item 10 was a third
sighting. One area (TERRAIN) owns all of it.

### 4. THE FIVE UNTAGGED ITEMS ARE BREADTH, AND THEY SPLIT INTO TWO KINDS

Per the user's call this round, they get **two** areas rather than one:

- **VARIETY** -- art breadth, no shape change: building variety and building
  depth, day/night skyboxes, start/finish banner variety, guardrail variety
  (G1, plus item 6). Low risk, high visible payoff, and the extraction pipeline
  already exists (`gen_trackgen_r5flora_tex.py`, `gen_trackgen_r7city_tex.py`,
  the confirmed finding that shipped tracks hold unmined art).
- **SHAPE** -- generated geometry changes: major height differences (G3),
  major long branches that diverge and rejoin (G5), slower lanes with distinct
  textures and driveable sidewalks (G2), highway-style branches (half of G4).

SHAPE is deliberately over-subscribed. **It must not attempt all four.** It picks
the two highest-value (height differences and long rejoining branches), ships
those, and writes a design note plus a backlog entry for the rest. Every one of
these changes the generated shape, so every one carries the round-4 risk: watch
for "boxed in", and confirm races still finish on both seeds.

- **BIOME** -- the eighth area, owning item 19 (snow-coherent seed selection,
  more biome kinds, one-side sea) and the "elements from every TD5 track" half of
  G4. Item 19's first clause is the tractable one and should lead: *if a seed
  includes snow, biome selection must prefer snowy themes* so the track does not
  lurch between snow and summer. That is a seed-level constraint on the biome
  roll, and it is cheap. Beaches, city sub-types and landmarks are breadth to
  scope down; the user explicitly offered to triage the "elements from every TD5
  track" list rather than have us guess, so BIOME's deliverable there is an
  INVENTORY to bring back, not a build.

### 5. AREA OWNERSHIP

| Area | Items | Seed(s) |
|------|-------|---------|
| GUARD   | 3, 7, 8            | 99991 (sweep both) |
| CROSS   | 1, 9               | 99991 (prove class on both) |
| CITY    | 2, 12, 6-adjacent  | 99991 |
| BRIDGE  | 4, 10, 11, 13, 17, 18 | both |
| TERRAIN | 5, 14, 15, 16      | both |
| VARIETY | G1, 6              | both |
| SHAPE   | G2, G3, G5, G4-part | both |
| BIOME   | 19, G4-part        | both |

Boundary calls worth stating, because two pairs of areas nearly collide:

- **Item 6 (building depth) is CITY-adjacent but owned by VARIETY.** The line is
  art versus massing: VARIETY owns how many distinct buildings exist and how deep
  they are, CITY owns where they stand and what stands next to them.
- **Item 12 (no city backdrop inside a park) is CITY, not GUARD.** It is a
  placement-validity rule, not an on-road overlap. Note that roadside parks are
  default OFF (`tg_block_is_park`) since the green-paved-crossing fix, so CITY
  must first turn parks on to reproduce it at all.
- **Item 17 (green medians inside tunnels) is BRIDGE, not TERRAIN**, because the
  tunnel bore owns what is legal inside it. Same family as item 12: a material
  chosen without asking what context it sits in.
- **Item 16 (snowy median, snow ground variety) is TERRAIN, not BIOME.** BIOME
  decides *whether* a run is snowy; TERRAIN decides what snow looks like.

### 6. METHOD, CARRIED FORWARD AND EXTENDED

The standing rules, unchanged:

1. **Byte attribution proves an emitter changed, never that the result is
   right.** Round 5 shipped four items on byte evidence; all four came back.
2. **A frame proves one instance at one span, not the class.** Round 6
   frame-verified four items that came back anyway -- one had photographed the
   bridge crown span where the fix genuinely worked while the complaint span 200
   units away was still broken.
3. **Photograph the span the user names, not a nearby one.**
4. **Confirm the emitter fired** (element inventory in race.log) before hunting
   pixels.

New for round 8:

5. **Two seeds, and a fix is not done until it holds on both.** Seed 777 has no
   CITY at all and the guard rejects nothing there, so an area that only ever
   loads 99991 is testing half its surface. Where an item is tagged to one seed,
   the FIX still has to be checked for collateral damage on the other.
6. **Do not inherit a prior round's diagnosis as fact.** R7 concluded the bridge
   pier height was already correct and the user has now contradicted that. Any
   claim of the form "round N established X" about an item that came back must be
   re-measured, not cited.

### 7. HARNESS TRAPS (unchanged, and each has bitten someone)

- **Kill only your own PID.** An agent ran `Get-Process td5re | Stop-Process` and
  killed the user's live session.
- **race.log flushes only on clean shutdown** and survives 5 rotations. Close the
  window; copy logs out immediately.
- **Worktree launches need CWD = the worktree root**, or the game silently runs
  the main repo's stale track. Prove liveness by A/B-ing a knob that exists only
  in your build.
- **`-fsyntax-only` does not report `-Wunused-function`.** The warnings ratchet
  (baseline 84) is checked only by `build_all.bat`, not by standalone lint.
- **Desktop capture is black** -- use `TD5RE_FRAMEDUMP=<abs path.png>`.
  `TD5RE_CAM_TOPDOWN` on a parked AutoRace sees otherwise-unshootable geometry
  but flattens billboards, so use chase view for flora. Matched-pose A/Bs require
  capturing at the same elapsed race time (countdown works).
- **`TD5RE_R7_GUARD` is default ON and drops meshes.** If new scenery goes
  missing, suspect the guard before suspecting your own emitter.

### 8. KNOWN OPEN ITEMS CARRIED IN

- 8 side-street mouths inside forks lack flanking pavement (R7 CROSS left this
  for the branch area; folds into R8 CROSS item 1).
- Span 900's one-span building on 99991 was never reproduced.
- Ponds requested in round 5 were never built (TERRAIN may fold this in).
