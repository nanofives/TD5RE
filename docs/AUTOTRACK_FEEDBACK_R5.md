# Autotrack feedback round 5 (2026-08-29)

Verbatim user feedback from a manual drive of the merged round-4 build plus the
item-12 fix (`dd5d0d77`, autotrack seed **99991**, track slot 60).
Rounds 1-4 (30 + 25 + 19 + 20 items) are merged; this is round 5, **18 items**.

Seed 99991 reference: CITY spans 0-449 and 600-899; forks at 144-169 (avenue,
sep 0.20), 319-360 (sep 0.59), 510-631 (sep 1.00, widest); corridors appended at
1801-1824 / 1826-1865 / 1867-1986; tunnels 480-499 and 1520-1539; bridge runs
1000-1039, 1160-1199, 1320-1359, 1640-1679.

## A. Banner

1. The pillars holding the start banner have a weird texture that's not fully
   covering it (bleeding the edges to another texture, I believe?).

## B. Street crossings

2. Street crossings have building facades on every "lane" of road spawned.
3. There's still elevated sidewalk on street crossings; those should wrap around
   the road.
4. On some crossings there's still a green texture that's around 1m height.
12. On span 934 the road crossing texture looks out of place when placed near
    this road texture.

## C. City massing / scenery placement

5. Skyscrapers are something that can be added, but on span 137 on the right
   there's a way too big skyscraper that takes a lot of space and looks weird.
6. On the branch on span 153 I see a background building texture that's
   colliding with the road itself.
7. On the right of span 160 there's a very stretched out texture of a building
   that's poorly placed diagonally to the branch road.
8. From span 181 up to span 202 there's plenty of elements that are clipping and
   overlapping over the road.
15. On span 1047 and zones around there's one span of the front of a building
    disconnected from the following spans.

## D. Tunnels

9. The tunnel on span 491 has walls that are not following the road, instead
   they are placed in a way that looks like slopes, and the texture looks wrong
   for a tunnel.

## E. Branches

10. On span 509 starts a branch; to the right side of the branch there's no
    sidewalk.

## F. Geometry safety (CRITICAL — REGRESSION)

11. There's still the issue of driving OOB on span 570.

## G. Bridges / water

13. On span 1001 there's some geometry going over the bridge that doesn't have
    any depth to it, looks 2D flat.
14. The water below span 1011 looks wrong because there's still plenty of road,
    building backgrounds mixed with grass. You must leave only water on the
    floor below bridges and create coastlines with sloped geometry.
17. The bridge on span 1179: the guardrails are not proper guardrail texture,
    and they have a gap between the guardrails and the road.

## H. Flora / parks

16. On span 1109 there's grass on the right, which is fine, but in the
    background at the end of the scenery the trees texture looks very stretched
    out and the height is not following the grass.
18. The biome around 1426 seems to be a park, which is fine, but if you are
    going to add a park with a lot of curves on the road, the curves should have
    a meaning. You can add small ponds, a lot of trees, trees that take up to
    the ceiling like the sections at the end of Moscow or the middle of
    Australia. We need more trees in all the different shapes and forms from the
    various tracks — you have to expand the library of available resources.

---

## Triage notes (orchestrator, 2026-08-29)

### ITEM 11 IS A FAILED FIX, NOT A NEW BUG. OWN IT.

Round 5 item 11 == round 4 item 12. It was investigated by a dedicated session,
root-caused, fixed (`dd5d0d77`), and reported as **VERIFIED**: "0 off-map frames
across full-throttle + off-line-steer escape attempts and a full AI race", with
the backstop observed firing once at span 1931. The user drove the merged build
containing that fix and **still escaped**.

So one of these is true, and the FIRST job is to determine which — do not start
by writing code:

1. **The rescue never armed.** `geo_query_active()` requires
   `s_track_custom_registry && !reverse && g_active_td6_level == 0`, and
   `s_track_custom_registry` is set inside `td5_track_bind_boundary_sentinels`
   from `td5_track_registry_has_level(level_number)`. If that classification is
   false in the user's actual launch path (different level number, registry not
   populated at that point, or the sentinel binder not called on this path), the
   whole fix is inert and every "0 off-map" measurement was taken on a code path
   the user never exercises.
2. **It armed but the user's escape is a different mechanism** — e.g. leaving
   from the branch/corridor side rather than the main-half, or exceeding
   `OOB_RESCUE_CAP` (4000/tick) so the pull-back loses to the escape velocity,
   or `geo_locate` failing to find any quad so the function returns early
   (`if (!geo_locate(...)) return;` — a car far outside the grid may not locate).
3. **The verification was not equivalent to the user's input.** The agent drove
   via the control socket; a human on a wheel/pad may reach a different speed or
   angle.

EVIDENCE TO GET FIRST: the run was launched with `TD5RE_OOB_DIAG=1`, which logs
an `OOB RESCUE` line whenever the backstop fires. The user's `race.log` from the
failing session therefore distinguishes (1) from (2) immediately:
- no `OOB RESCUE` line at all + car went off-map -> the backstop never fired;
  then check whether it was even armed (log `geo_query_active` / the registry
  classification at load).
- `OOB RESCUE` lines present but the car still left -> the backstop fires but is
  too weak or too late; tune `OOB_RESCUE_DIST` / `OOB_RESCUE_CAP`, or catch it
  earlier.

**Do not tune constants before knowing which case it is.** This is the
"verify the crux claim" failure mode: a confident VERIFIED that did not match
the user's reality.

### Items that land on round-4 work reported done

- **Item 1 (banner pillars)** — r4-FLOW squared off the legs for item 1's
  "pillars don't look solid". The geometry complaint is resolved; the complaint
  is now TEXTURE: not fully covering, edges bleeding into a neighbouring page.
  That is a UV/page-extent problem on the leg prism, the same class as the
  banner panel's half-texel inset. Likely the legs sample a page region that
  isn't theirs.
- **Item 4 (green on crossings)** — r4-CROSS fixed "zebra painted into park
  gaps" and verified crossings dropped 396->385 with park spans gone. The user
  still sees green, and now adds a crucial detail: it is **~1m HIGH**, i.e. a
  raised object, not a flat decal. That is a different object from the one CROSS
  fixed. Suspect the raised kerb break (item 2 of round 4) or a park hedge
  emitted at the crossing. Confirm the object before touching CROSS's fix.
- **Item 17 (bridge guardrails)** — r4-BRIDGE replaced the scrambled parapet
  page with `TD5_TG_PAGE_R4_GUARDRAIL` and frame-verified it "reads as a
  concrete barrier". The user says it is still not a guardrail texture AND adds
  a NEW defect: a **gap between the guardrail and the road**. The gap is
  geometry, not texture, and was not present in the round-4 report.
- **Item 14 (water under bridges)** — r4-BRIDGE's coastline was explicitly
  reported UNVERIFIED VISUALLY (no side view of the gorge). The user has now
  supplied the missing observation: under the bridge there is still road,
  building backgrounds and grass instead of water. So the coastline emitter
  fires (inventory proved 8 strips) but the ground beneath a bridge is not being
  cleared. **This is the unverified claim coming due — treat the emitter as
  fine and the ground-clearing as missing.**
- **Item 10 (no sidewalk right of the branch at 509)** — r3-branch shipped
  "branch OUTER pavement on corridor spans" and r4 inventory showed
  `sidewalks last=1986` covering corridor spans. But r3 also flagged that the
  **AI never leaves the main ring, so branch pavement was never seen by a
  human**. This is that blind spot resolving into a real defect. Inventory
  presence != correct side.
- **Item 5 (oversized skyscraper at 137)** — this is r4-CITY's `fork-back`
  emitter (`tg_city_emit_forkback`, runs 136-171). The orchestrator flagged at
  merge time that it "reads as a fairly dense/tall canyon wall, plausibly wants
  tuning shorter/sparser". The user has now confirmed exactly that. Tune scale
  and footprint; do not remove the feature (item 6 of round 4 asked for it).
- **Item 9 (tunnel at 491)** — r3-bridge shipped 5 tunnel lining variants and
  r4 kept them. Two distinct complaints: lining texture still wrong, AND the
  walls do not follow the road (they read as slopes). The second is geometry and
  is the more serious of the two.

### New-generation work (changes shape / needs assets)

- **Item 18 is the largest single request of the round** and is an ASSET
  SOURCING task as much as a generation one: more tree varieties, tall
  canopy trees comparable to the end of Moscow / middle of Australia, ponds, and
  curves that justify themselves. The existing extraction tooling lives in
  `re/tools/` and shipped track assets in `re/assets/`. Expect this to need its
  own owner and its own round; do not let it crowd out the correctness items.
- **Item 3** (sidewalk wrapping around the crossing) and **item 2** (facades on
  every spawned lane) are both about crossings being generated per-lane rather
  than per-intersection. Suspect ONE cause behind both.
