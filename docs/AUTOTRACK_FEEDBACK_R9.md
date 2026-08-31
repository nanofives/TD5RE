# Autotrack feedback round 9 (2026-08-30)

Verbatim user feedback from a manual drive of the merged round-8 build
(`4439df11`, seed **99991**, track slot 60). Rounds 1-8
(30+25+19+20+18+19+19+24) are merged; this is round 9, **13 items**, all on
seed 99991. Three screenshots accompany the report (spans 475, 549, 1002).

Seed 99991 reference: CITY 0-449 and 600-899, ORIENTAL 450-599, INDUSTRIAL
900-1049, COAST 1050-1199, ORIENTAL 1200-1499. Forks 144-169 / 319-360 /
510-631. Tunnels 480-499 / 1520-1539. Bridge runs 1000-1039 / 1160-1199 /
1320-1359 / 1640-1679.

## Verbatim items

1. the crossing on span 66 this one in particular is still rendering buildings
   on the outer part of the sidewalk, making the sidewalk not visible
2. the right track on span 150 branch has double sidewalk
3. the park to the right of span 358 should have buildings on its three sides
4. there are no sidewalks on the merging of the branch on span 361
5. the entrance of the tunnel still have wrong textures on top and on its sides
   (screenshot 153604), also there's something wrong with the depth of the
   entrance pillar, the gray-ish texture looks wrong here, the span that
   represents the entrance and exit of the tunnel looks wrong, also walls and
   roofing should have different texture
6. on span 549 there's no geometry on the right side of the road (screenshot
   154228) i realized because there's a very inclined slope that starts before
   the road finishes and the trees are not following, if you make a sloped side
   make sure that it goes further than usual
7. the nearby geometry on span 617 looks wrong in many ways, i believe because
   this is a U-turn and there has to be nearby geometry on the other side of the
   curve, but when doing this you gotta take into consideration if the nearby
   geometry is touching another road and a downwards slope, **all geometry has
   to be connected through the same topographic logic**
8. sometimes like on span 957 there's double guardrails?
9. the structure that goes over the bridge should be connected with the
   structure below (screenshot 154625) also the texture for these pillars should
   be diferente, and the horizontal beam should have a different texture as
   well, the coastline geometry looks wrong
10. on span 1039 there's buildings on the background that are over the water
11. the transition around span 1050 is grass in between tiles, this is wrong
12. in the middle of the bridge on span 1191 there's different guardrails, are
    these supposed to be upside down?
13. the tunnels in the city should represent underpasses below highways, build
    it like that, and current tunnels should only be happening in mountains
    biome

---

## Triage notes (orchestrator, 2026-08-30)

### 0. ITEMS 8 AND 12 ARE AN ORCHESTRATOR-CAUSED REGRESSION. FIX FIRST.

At the R8 merge I flipped `TD5RE_AUTOTRACK_GUARDRAILS` from opt-in to default
ON, because R8 VARIETY had built four guardrail pages for G1 ("different
guardrails") and with the emitter off that whole axis shipped invisible. I
measured the flip before making it -- 1178 rails over 589 spans, guard rejects
unchanged at 69, road/deck invariant 0, race to span 1699 -- and none of those
checks could see a DOUBLED rail, because every one of them counts meshes or
rejections rather than asking whether two rails occupy the same edge.

Confirmed cause for item 12: `tg_span_needs_guardrail_raw` contains
`if (tg_span_in_bridge_run(si)) return 1;` -- the roadside emitter deliberately
rails the whole bridge run -- while BRIDGE's parapet rails the same deck. With
the emitter now on by default, every bridge deck gets both, at two different
treatments, which is also why the user reads one of them as "upside down".

**Item 8 is NOT the same case and must not be assumed to be.** Span 957 lies in
no bridge run on this seed (runs are 1000-1039 / 1160-1199 / 1320-1359 /
1640-1679). So there is a SECOND doubling mechanism -- most likely the
"elevated" rule and the deck rule both firing, or the raw predicate and its
`si+k` lookahead smear both emitting. Two mechanisms, one area.

Note what this says about the R8 acceptance evidence: a count going up is not
evidence of correct placement. "1178 rails emitted" was true and useless.
RAILFIX's acceptance must be a per-edge uniqueness check -- no road edge carries
two rail meshes -- swept over both seeds, not a mesh count.

### 1. ITEM 7 IS THE ROUND'S SYSTEMIC ITEM, AND THE USER HAS NAMED THE PRINCIPLE

> "all geometry has to be connected through the same topographic logic"

Items 6, 7 and 11 are the same defect seen three ways: adjacent pieces of world
are each individually plausible and mutually inconsistent.

- **Item 6 (span 549)**, with its screenshot, gives the clearest mechanism. The
  verge drops on a steep slope that BEGINS BEFORE the road ends, the ground dies
  into void shortly after, and the trees stay on the ridge line instead of
  following the slope down. R8 TERRAIN measured extent HORIZONTALLY and reported
  span 549 R going 12000 -> 30000. That measurement was honest and the complaint
  survived it, because a steep verge spends its reach VERTICALLY: 30000 units of
  horizontal allowance buys very little visible ground once the surface is
  falling away. The user's own instruction is the fix -- "if you make a sloped
  side make sure that it goes further than usual", i.e. extent must be measured
  along the surface, not across the map.
- **Item 7 (span 617)** is a U-turn where the inside of the curve needs ground
  that agrees with the road it faces AND with the slope under it.
- **Item 11 (span 1050)** is grass between tiles at a biome transition -- two
  neighbouring surfaces that never asked what the other one was.

This is R7's "add a way to check for things on the road" one level up, and it
should be treated the same way: build the shared authority once (a topographic
continuity rule that neighbouring emitters consult), not three local patches.
Note that R8 already proved the value of this shape of fix twice -- the on-road
guard, and BRIDGE's discovery that `tg_emit_ground` used ONE cross-section for
BOTH slab edges, which cracked the ground at every span boundary. Item 11 in
particular smells like a sibling of that bug.

### 2. TUNNELS ARE A FOURTH-ROUND ITEM AND THE SCREENSHOT FINALLY EXPLAINS WHY

R6 built a new portal page, R7 built a concrete portal facade projected past the
buttress, R8 replaced the banded page 301 with the flat page 352 and verified
the SELECTION at all 12 mouths. Each round fixed something real and the
complaint returned, because **all three rounds were working on the portal FACE
while the surround was never portal material at all.**

Screenshot 153604 (span 475) shows it plainly: the band above the lintel and the
wings either side are a mottled green-grey HILLSIDE/terrain texture, not
concrete. The portal reads wrong because the hill is wearing the portal's
silhouette. That is a different object from the one three rounds have been
re-texturing.

The item carries four more specifics, all visible in the same shot:
- **jamb depth** -- the face reads as a thin slab with no reveal
- **the grey-ish face texture** is still not liked (this is the R8 page 352)
- **the entrance/exit transition span** looks wrong
- **walls and roofing share one page** -- the bore interior needs distinct
  ceiling material

**Item 13 is a design rule, not a defect, and it belongs with this area:** city
tunnels should be built as UNDERPASSES BENEATH HIGHWAYS, and true bored tunnels
should only occur in mountain biomes. That reframes the whole element rather
than re-texturing it, and it is very likely the right answer to four rounds of
"the tunnel entrance looks wrong" -- a city tunnel mouth has been trying to look
like a mountain portal, which is why no portal art ever satisfied it. Do item 13
FIRST and re-judge item 5's remaining specifics afterwards.

### 3. GOOD NEWS TO CARRY FORWARD

**R8 item 10 (bridge pillars, three rounds open) is RESOLVED.** Screenshot
154625 at span 1002 shows the piers running down into the water. It should be
closed, and BRIDGE should not spend any of round 9 on it.

### 4. THE REST ARE PLACEMENT, AND TWO ARE R8 REGRESSIONS TO CHECK

- **Item 1 (span 66)** is the FOURTH round on crossings and the SECOND on this
  exact symptom -- R8 CROSS moved the reveal row from 7100 to 22700 precisely so
  massing would stop crowding the mouth, and reported 190 mouths swept. The user
  says span 66 "in particular", so treat it as a case the class sweep did not
  cover rather than a total failure, and find out what is special about it.
- **Item 2 (span 150, double sidewalk on a branch)** is plausibly caused by R8
  CITY replacing the binary `tg_pavement_side_blocked` with a WIDTH
  (`tg_pavement_side_width`), which both the raised city slab and the verge band
  now read. If both now emit where previously one was suppressed, that is a
  double pavement. Check that first.
- **Item 4 (no sidewalks at the branch merge, span 361)** is the rejoin end of
  the same fork whose entry R8 CITY fixed at 143. Likely the same off-by-one at
  the other end.
- **Item 3 (park at 358 wants buildings on three sides)** is composition, and
  note parks are DEFAULT OFF (`tg_block_is_park`) -- confirm what the user is
  actually seeing before building.
- **Item 10 (buildings over water at 1039)** is a placement-validity rule, the
  same family as R7's "no trees over water" and R8's "no city backdrop in a
  park". 1039 is the last span of bridge run 1000-1039.
- **Item 9 (span 1002)** wants the over-bridge structure to LAND on the piers
  below, plus distinct pillar and beam textures, plus coastline geometry. The
  connection request is the substantive half.

### 5. AREA OWNERSHIP (6 areas, user's call)

| Area | Items | Note |
|------|-------|------|
| RAILFIX  | 8, 12       | Orchestrator regression. Two mechanisms. Per-edge uniqueness is the test. |
| TUNNEL   | 5, 13       | Do 13 first, then re-judge 5. Fourth round. |
| TOPO     | 6, 7, 11    | Systemic. Build the shared authority, not three patches. |
| BRIDGE   | 9, 10       | R8 item 10 (piers) is CLOSED -- do not re-open. |
| CITY     | 1, 2, 3, 4  | 2 and 4 are likely R8 regressions/off-by-ones. |
| INFRA    | bitmap + backlog | See below. |

INFRA's `s_acct_mask` widening is done in the PREP COMMIT rather than by the
area, because every other area needs to build on the widened mask. It is now
SELF-SIZING off `TG_ACCT_KIND_COUNT` with a compile-time assert, so it can never
silently overflow again. INFRA instead owns the deferred backlog: ponds (asked
in R5, still unbuilt), landmarks, slower lanes + driveable sidewalks (R8 SHAPE
noted the lane mask byte already exists and is deliberately held at zero, and
that it collides with the on-road guard's authority), highway-style branches,
and the R8 BIOME asset inventory's cheapest wins -- 12 already-extracted
breakable street-furniture meshes in `re/assets/props/` that nothing places.

### 6. METHOD, CARRIED FORWARD

Unchanged and still earning their keep:

1. Byte attribution proves an emitter changed, never that the result is right.
2. A frame proves ONE instance at ONE span, not the CLASS.
3. Photograph the span the user names, not a nearby one.
4. Confirm the emitter fired (element inventory in race.log) before hunting pixels.
5. Two seeds -- a fix is not done until it holds on both.
6. Do NOT inherit a prior round's diagnosis as fact. R8's GUARD area disproved
   the orchestrator's own triage by measuring it, and was right to.

New for round 9, from this round's own evidence:

7. **A COUNT GOING UP IS NOT EVIDENCE OF CORRECT PLACEMENT.** The guardrail
   regression passed every R8 check because they all counted meshes or
   rejections. Where the defect is "two things in one place", the acceptance
   test must be a UNIQUENESS check over the shared resource (this edge, this
   surface, this span-side), not a total.
8. **When a complaint survives an honest measurement, suspect the AXIS of the
   measurement.** Span 549's extent was measured horizontally and genuinely
   doubled; the ground still ends too close because the reach is spent
   vertically. Four rounds of tunnel work re-textured the face while the
   surround was terrain. In both cases the number was true and measuring the
   wrong thing.

### 7. HARNESS FACTS (accumulated, all cost someone time)

- `TD5RE_*` env knobs PERSIST across launches in one PowerShell session -- clear
  them before EVERY launch or an "isolated" A/B silently inherits earlier knobs.
- `--StartSpanOffset=N` parks the car at span **N+15**.
- `TD5RE_FRAMEDUMP` rewrites every 30 frames, so a long wait captures wherever
  the car drifted to, not the span you asked for.
- **AutoRace does NOT exit the process on finish** -- it sits on the results
  screen, so a WaitForExit harness always "times out". The killed race.log still
  retains the element inventory.
- race.log flushes fully only on clean shutdown, and survives 5 rotations.
- Worktree launches need CWD = worktree root, or the game runs the main repo's
  stale track. Prove liveness by A/B-ing a knob unique to your build.
- Desktop capture is black; use `TD5RE_FRAMEDUMP=<abs path.png>`.
  `TD5RE_CAM_TOPDOWN` flattens billboards; oblique cams
  (`TD5RE_CAM_TOPDOWN_SIDE/_BACK/_TGTDROP`) see under a bridge deck.
- `td5_env_flag_on()` defaults to 1 when UNSET; `td5_env_flag_off()` returns 1
  only for a literal "1" and is the OFF-by-default idiom.
- Kill ONLY your own PID.
