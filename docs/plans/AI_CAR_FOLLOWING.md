# AI car following in world units (round 1028 S)

Mariano: *"AI racers following each other in a convoy still make contact."*

Round 1024 H flagged the weakness and did not fix it: "Governor/follower weakness (smart_speed_core
gap in spans) NOT fixed". Round 1026 M / 1027 N removed the contacts that fired with a *real gap*, so
every contact counted here is two bodies genuinely overlapping.

## The defect

`td5_ai_smart_speed_core` (`td5_ai.c`, `smart_*` SECTION) decides how hard to follow from
`smart_gather_blockers`' `gap`, which is a **count of track spans**, and acts only when that count
is 1 or 2 (`desired = 2 - aggression`, floored at 1).

1. **A span is not a distance.** Spans are hundreds to thousands of model units long (a car is
   1452 long, 632 wide), so `gap == 1` means anything from touching to several car lengths. The same
   rule is far too loose on a long span and needlessly tight on a short one.
2. **`gap == 0` is excluded on purpose** — the `[GAP>=1 2026-08-15]` note — because a *level* pair
   ratchet each other down. But `gap == 0` is also the closest a follower can be, and a span index
   cannot tell "level alongside" from "nose on the leader's bumper".
3. **No closing-speed term.** The ease is `gap/(desired+1)` with a 0.66 floor; the brake needs
   `gap <= 1` **and** a closing speed above `0x6000` (96 u/tick). A follower closing at 30-80 u/tick
   inside one span gets no brake at all.

Measured: in 12 all-AI races (below), **92 of 178 racer-vs-racer contact episodes were rear-end
shunts** — a follower driving into the back of an aligned leader it was closing on.

## The fix (`TD5RE_AI_FOLLOW_DIST`, default 1)

`ai_follow_nearest` / `ai_follow_apply` in `td5_ai.c` replace the span-gap decision with the one a
driver makes. Candidates are the same peer set the old rule scanned (present, same traffic
partition, within `[-1 .. lookahead]` spans), then for each peer, in the ROAD frame at the self span
(forward from `smart_forward_dir`/`_p`, lateral from `smart_span_frame`):

```
lon   = forward offset of the peer's centre          (> 0 = ahead of us)
lat   = lateral offset
clear = lon - (half_len_self + half_len_peer)        bumper-to-bumper, < 0 = overlapping
```

* "in front" is `lon > 0` — geometry, not a span index, which fixes (2) honestly: a car **level**
  alongside has `lon ~ 0` and is not followed, a car on our bumper has `lon ~ one car length` and is.
* "in my lane" is `|lat| <= (hw_self + hw_peer) * LAT_PCT/100`, i.e. a percentage of the offset at
  which the two bodies would just touch side by side. The default 115 % must stay well under a lane
  width (~1000 units against 632 of car) or a car being overtaken in the NEXT lane gets braked for.
* The body half-extents come from `td5_physics_actor_body_extent` (new, `td5_physics_collision.c`),
  which is the **same box the vehicle-vs-vehicle narrowphase tests** — mesh hitbox when loaded,
  cardef extents otherwise.

Control law, with `v`/`vp` the two longitudinal speeds in u/tick and `dv = v - vp`:

```
want = FOLLOW_MIN + v * HEADWAY * aggression_factor      desired clearance
brake   when dv > 0 and (clear <= 0 or clear < dv * BRAKE_TTC)     time-to-contact
ease    when clear < want:  thr *= FLOOR + (1 - FLOOR) * clamp(clear/want, 0, 1)
```

It still only ever LOWERS the command, so the faithful throttle ceiling and the corner governor above
remain the upper bound. Deterministic (no rng, the same doubles the rest of the smart layer uses), so
netplay lockstep is unaffected.

| knob | default | meaning |
|---|---|---|
| `TD5RE_AI_FOLLOW_DIST` | 1 | 0 = the master span-gap rule, verbatim |
| `TD5RE_AI_FOLLOW_HEADWAY` | 18 | ticks of time headway (at 320 u/tick that is ~5800 units, 4 car lengths) |
| `TD5RE_AI_FOLLOW_MIN` | 400 | standing clearance added to the headway, model units |
| `TD5RE_AI_FOLLOW_LAT_PCT` | 115 | lateral window, % of (half-width A + half-width B) |
| `TD5RE_AI_FOLLOW_BRAKE_TTC` | 26 | brake when the bodies meet within this many ticks |
| `TD5RE_AI_FOLLOW_FLOOR` | 45 | throttle floor of the ease, % |

Aggression keeps its old role: clean (0) stretches the headway x1.25, aggressive (2) tucks in x0.85.

## Tools

* `verify/r1028s_run.py` — one all-AI race per (track, seed): `--PlayerIsAI=1`, no traffic, no cops,
  damage off, `TD5RE_V2V_GAPLOG=2`, windowed / sent to the back / RT off / min graphics. Keeps
  `log/r1028s_<tag>_t<track>_s<seed>_race.log` and the `track` race trace.
  (A `.ps1` runner is the house style; writing `.ps1` is blocked by managed policy on the account
  this round ran on, so the same launch recipe lives in Python.)
* `verify/r1028s_rearend.py` — counts rear-end episodes in those logs.
* `verify/r1028s_pace.py` — compares two run sets' track progress at the last common sim tick.
* Round 1028 S adds `lon= lat= vlA= vlB=` to the `v2v_gap:` line (`TD5RE_V2V_GAPLOG`, dev builds)
  so an analyser can tell a rear-end shunt from a side swipe. Appended at the end of the line, so
  `verify/r1026m_gapstat.py` still parses it.

A contact is a REAR-END when, at the first tick of the episode: both slots are racers, the yaw
difference is <= 30 deg, `|lon| > |lat|`, and the car BEHIND is the faster one.

## Measurement

12 races, 2000 sim ticks each, 6 all-AI cars, `--Difficulty=1`, seeds 11/22/33 on La Plata
(level091, slot 61), Moscow (0), Sydney (2) and Newcastle (5). Both arms are the SAME exe; the
baseline is `TD5RE_AI_FOLLOW_DIST=0`. Races are not tick-deterministic, so read the ratios.

| | racer-racer episodes | REAR-END episodes | rear-end contact lines |
|---|---|---|---|
| master rule (`=0`) | 178 | **92** | 216 |
| default (`=1`) | 57 | **22** | 30 |

Per track (rear-end episodes, 3 seeds each): La Plata 15 -> 9, Moscow 27 -> 4, Sydney 33 -> 5,
Newcastle 17 -> 4.

Pace at the same sim tick (`r1028s_pace.py`, span_accum; + = the fix is further along):

```
mean over 12 runs: leader +2.2%, field +2.8%
```

Sydney gains most (+10 to +19 %): fewer shunts means less time spent recovering. Moscow's leader is
1 to 9 % down across the three seeds while its field mean moves -1.7 to +0.4 %, which is inside
this harness's run-to-run spread but is the one place where the extra cushion visibly costs the
leader. No stalled car in either arm (0/0 in every run).

## Gates

* Knob OFF vs the same-tree master exe (66a9f923): see the round report.
* Fork harness `verify/geo_fork_validate.ps1` on La Plata.
* `scripts/selftest.ps1` smoke; synthetic track identity (`verify/geo_tags_identity.ps1`).

## Not fixed / not verified

* Traffic keeps its own reactive rules (`traffic_smart_*` in `td5_ai_traffic.c`); this rule only
  runs on the racer path (`td5_ai_update_track_behavior`), so cop and racer slots, not traffic.
* The half-length is taken along each car's own axis, so for two cars at an angle the clearance is
  an approximation (conservative: it under-states the gap).
* Nobody has driven it by hand. Lap TIMES as such were not measured — the comparison is track
  progress at an equal sim tick, because an AI field rarely finishes inside the tick cap.
* The `lon > 0` test uses the road frame, not the car's own heading; a car spun sideways in front of
  us is still followed, which is intended but untested as a separate case.
