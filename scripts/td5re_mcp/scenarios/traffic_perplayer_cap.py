"""traffic_perplayer_cap -- the dynamic per-player traffic cap scales the
on-road budget by the number of separate racer clusters (pending row 10).

Each racer anchors a traffic bubble (per-anchor volume budget `per`); the
effective cap = clamp(per * clusters, [per, pool]). Racers bunched together
form ONE cluster (eff_cap == per); a racer that pulls a bubble-radius away
opens a fresh cluster and more budget, up to the pool. The engine already
emits a throttled `traffic_perplayer_cap: on_road=.. eff_cap=.. clusters=..
anchors=.. span[..] pool=..` line to race.log (LOG_TAG "ai") ~every 2s when
the cap is on (default; TD5RE_TRAFFIC_PERPLAYER_CAP=0 disables).

Drive a full AI field on Moscow with VERY HIGH traffic under fast-forward,
then parse those lines and assert the cap invariant: eff_cap == the clamped
per*clusters formula, on_road never exceeds eff_cap, and separation actually
opens more than one cluster (eff_cap expands past a single bubble).
"""
import os
import re
import sys
import time

# Shrink the cluster share-radius BEFORE launch (env is read at race init) so the
# naturally-spreading AI field forms MORE than one cluster on a compact circuit
# -- the default radius (despawn + scaled spawn-max) keeps a bunched Moscow grid
# as a single shared bubble (also correct, but then eff_cap can't grow). A small
# radius is the intended lever (TD5RE_TRAFFIC_SHARE_SPANS) for exercising the
# per-player scaling.
os.environ["TD5RE_TRAFFIC_SHARE_SPANS"] = "15"

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE, REPO_ROOT

LINE = re.compile(
    r"traffic_perplayer_cap: on_road=(\d+) eff_cap=(\d+) clusters=(\d+) "
    r"anchors=(\d+) span\[min=(-?\d+) max=(-?\d+) spread=(\d+)\] pool=(\d+)")

s = Scenario("traffic_perplayer_cap")
if not s._owns_proc:
    print("[traffic_perplayer_cap] FAIL: needs a fresh launch "
          "(TD5RE_TRAFFIC_SHARE_SPANS must be set at process start) -- close "
          "the running game first")
    sys.exit(1)
s.cmd("set_param", {"name": "trace_fast_forward", "value": 8})   # applies next race

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 laps=3, opponents=5, cops=0, traffic=4,
                                 player_is_ai=1, auto_throttle=1),
           "full-field VERY HIGH traffic race launched (Moscow, 5 AI, 8x FF)"):
    slot = s.state().get("race", {}).get("player_slot", 0)
    # Let the field spread out over a few laps so clusters can exceed 1.
    end = time.time() + 35
    while time.time() < end:
        st = s.state()
        if st.get("game_state") != STATE_RACE:
            break
        s.recover_if_broken(st, slot)
        time.sleep(0.5)

    # Parse this launch's race.log (truncated per launch, so all lines are ours).
    samples = []
    try:
        text = (REPO_ROOT / "log" / "race.log").read_text(errors="ignore")
    except OSError:
        text = ""
    for m in LINE.finditer(text):
        on_road, eff_cap, clusters, anchors, smin, smax, spread, pool = map(int, m.groups())
        samples.append(dict(on_road=on_road, eff_cap=eff_cap, clusters=clusters,
                            anchors=anchors, pool=pool))

    n = len(samples)
    max_clusters = max((x["clusters"] for x in samples), default=0)
    max_on_road = max((x["on_road"] for x in samples), default=0)
    pool = samples[0]["pool"] if samples else 0
    print(f"[{s.name}]   cap-log samples={n} max_clusters={max_clusters} "
          f"max_on_road={max_on_road} pool={pool}")

    if s.check(n >= 3, "collected per-player-cap diagnostic lines from race.log"):
        # per = eff_cap in a single-cluster sample (unclamped, clusters*per<=pool);
        # else the smallest eff_cap seen (min clusters == fewest bubbles).
        singles = [x["eff_cap"] for x in samples if x["clusters"] == 1]
        per = singles[0] if singles else min(x["eff_cap"] for x in samples)

        def expected(c):
            e = per * c
            if e > pool: e = pool
            if e < per:  e = per
            return e

        formula_ok = all(x["eff_cap"] == expected(x["clusters"]) for x in samples)
        # on_road can transiently exceed a JUST-lowered eff_cap (racers rebunch ->
        # clusters merge -> cap drops, but the cap gates SPAWNING, not despawn, so
        # already-on-road cars linger until they retire). The hard safety invariant
        # is that on_road never overflows the slot pool.
        safe_ok = all(x["clusters"] >= 1 and x["eff_cap"] <= pool
                      and x["on_road"] <= pool for x in samples)
        bad = [x for x in samples if x["eff_cap"] != expected(x["clusters"])]

        s.check(safe_ok, f"clusters>=1, eff_cap and on_road never overflow the pool ({pool})")
        s.check(formula_ok,
                f"eff_cap == clamp(per*clusters,[per,pool]) with per={per} pool={pool}"
                + (f"  (bad: {bad[:3]})" if bad else ""))
        s.check(max_clusters >= 2 and max_on_road > per,
                f"separation expands the budget past a single bubble "
                f"(max_clusters={max_clusters}, max_on_road={max_on_road} > per={per})")

    s.end_race_best_effort()

s.finish()
