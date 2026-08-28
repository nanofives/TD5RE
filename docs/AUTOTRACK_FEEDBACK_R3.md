# Autotrack feedback round 3 (2026-08-28)

Verbatim user feedback from a manual drive of the merged round-2 build
(`1f930001`, autotrack seed **99991**, which draws two CITY runs: spans 0-449 and
600-899). Rounds 1 (30 items) and 2 (25 items) are merged; this is round 3.

Drive conditions: dev build, RT HIGH (default), `time=DAY` for this seed, user
driving (no AI override).

## A. Buildings / city massing

1. On a city section, buildings have facade and roofing but they don't have side
   walls. Buildings with different height look like they are hollow inside.
2. I see a lot of rows of buildings behind buildings, but that should only happen
   when there's a street passing through buildings. Streets can be pedestrian
   (narrow) or for vehicles (wider, up to 4 lanes).

## B. Street intersections

3. Street crossings are there but you need to actually make a street
   INTERSECTION in these places. Street intersections should have streets
   crossing; guardrails and sidewalks should turn according to the street; there
   shouldn't be buildings on the crossing. Investigate if the crossings can be
   diagonal too, to give more variance. Street crossings can be on one side, the
   other, or both.
4. On cities there could be a 90 degree turn that represents "around the block"
   as the map continues.

## C. Parks

5. Sometimes on cities there could be parks with green on the side. Make sure to
   add nearby streets, sidewalks and buildings to make it fully immersive (on one
   or both sides).
6. On parks you can add individual houses.

## D. Guardrails

7. On a city section guardrails are missing on some spans. They should look
   continuous and only break if there's a change in biome or a street crossing.

## E. Flora

8. Some background continuous tree lines look gray at the bottom and white at the
   top, and it doesn't seem to be a tree texture there.

## F. Branches / avenues

9. There's still missing gaps between road and sidewalk on branches. Buildings
   are spawned ON the branches. There should be a distinction of different
   dividers in the middle if the branch represents an avenue.
10. Remember that you can add various sizes of separation of branches, and
    geometry should be handled accordingly.

## G. Bridges

11. Avoid tiled roads on bridges.
12. Bridge guardrails look like steps instead of an inclined guardrail.
13. Bridges are like small bumps all the time. There should be a tweak to make
    the pre-bridge slopes bigger, the bridge itself longer, and there should be
    optional structure above.
14. On bridges I see patches of grass. Grass slopes are not connecting properly
    with water.
15. Avoid sharp turns on bridges.

## H. Tunnels

16. Tunnel texture is always the same, it looks wrong and very repetitive. Don't
    add tunnels to plain cities, only on mountains.

## I. Geometry safety / race flow

17. Add additional checks to avoid geometry of floor and walls getting in the way
    of the road.
18. Race is not finishing on the finish line.
19. Add an end-of-road wall that avoids the car leaving the map.

---

## Triage notes (orchestrator, 2026-08-28)

### Three of these land on work that was reported as done. Own that.

- **Item 8 (grey/white tree lines) is a REPEAT of round-2 item 14.** r2-flora
  claimed it by flipping `TD5RE_AUTOTRACK_REAL_TEX` to default ON and rebuilding
  the tree-line band from a real canopy page. That was merged but recorded as NOT
  confirmed in frame, and this report confirms it did not work. Re-open, and this
  time verify with a framedump of the band before claiming it.
- **Item 9 (buildings on branches, road/sidewalk gaps) lands on the post-merge
  carriageway work.** `tg_flora_branch_reach` and `tg_ground_branch_clear` were
  retired onto the shared `tg_carriageway_reach` specifically to stop scenery
  landing on widened branches. Flora and ground skirts were covered; FACADES were
  not routed through the authority. That is the likely gap -- `tg_emit_street_wall`
  / the city hooks need the same `tg_on_carriageway` / `tg_carriageway_clear_gap`
  treatment.
- **Item 18 (race not finishing) contradicts a CONFIRMED claim.** It was verified
  end to end on seed 20260827 with `--PlayerIsAI=1`: LEVELINF checkpoints loaded,
  all 6 racers logged `Actor finish`, `Race finished -> results screen`. On THIS
  run the checkpoint data is still correct
  (`Checkpoint record from LEVELINF: track=60 count=4 spans=443,862,1281,1700`)
  but **no `Actor finish` and no checkpoint-pass events were logged at all**.

### Item 18: what is actually known, and what is NOT

KNOWN:
- Checkpoint spans load from LEVELINF correctly (443 / 862 / 1281 / 1700).
- Zero checkpoint-pass events in race.log for the whole session.
- The highest span touched was **1986**, and spans **1801-1986 are BRANCH
  CORRIDOR spans** appended after the 1800-span main ring
  (`branch-nodes runs[6]: 144-169, 319-360, 510-631, 1801-1824, 1826-1865,
  1867-1986`). So the high span number does NOT prove the finish was crossed.

NOT KNOWN (do not assume):
- Whether the car crossed span 1700 ON THE MAIN RING at all.
- Whether the failure is checkpoint-pass detection, or progress mapping while on
  a corridor.

STRONGEST HYPOTHESIS, matching the difference between the two runs: the verified
run was AI-driven and stayed on the main line, whereas a human can drive onto a
branch corridor, whose spans (1801+) are not main-ring positions. If progress /
checkpoint crossing keys off the raw span index, a driver on a corridor never
registers the checkpoint spans. This also sits directly alongside items 9 and 10.

FIRST STEP: instrument, do not guess. Log every checkpoint-pass test with the
player's raw span, the corridor-vs-main-ring flag, and the mapped main-ring span
(`tg_fork_of_corridor` exists for exactly this mapping). Then drive the main line
without touching a branch and see whether it finishes -- that single A/B splits
"checkpoint detection is broken" from "corridors break progress".

### Cross-cutting engineering notes

- Items 3, 4, 5, 6, 13 and 19 are NEW GENERATION features, not fixes: real
  intersections, 90-degree block turns, parks with surrounding streets/houses,
  longer bridges with approach slopes, and a map-edge wall. These change track
  SHAPE, so expect the same class of risk as r2-branch item 20 (tracks may come
  out shorter, watch for "boxed in" in race.log).
- Items 1, 11, 12, 14, 16, 17 are correctness/appearance fixes on existing
  emitters and are much lower risk.
- Item 16 (tunnels only on mountains) is partly already expressible: the biome
  weighting is live (`tg_biome_tunnel_pct`, CITY is 60%). Setting CITY's
  `w_tunnel` to 0 is a one-value change; the repetitive lining texture is the
  separate half of that item.
- **Item 7 is NOT about guardrails, and the root cause is already known.** The
  element inventory for this exact run reports `NONE emitted: lamps guardrails`,
  so zero guardrails were generated (they are default OFF:
  `tg_guardrails_enabled` -> `td5_env_flag_off`, opt-in via
  `TD5RE_AUTOTRACK_GUARDRAILS=1`). What the user saw breaking up along a city
  kerb is the PEDESTRIAN RAILING from `tg_city_emit_fence` (accounted as
  `TG_ACCT_FENCE`; 1521 of them on this seed).
  Its own comment explains the gaps: an independent per-side hash gate
  deliberately "drops roughly a third of spans", written to break the railing
  into runs. The user is asking for the opposite rule -- continuous, breaking
  ONLY at a biome change or a crossing. So the fix is in that hash gate, not in
  the guardrail emitter, and it is close to a one-function change. Do not go
  looking at `tg_emit_guardrail` for this.
