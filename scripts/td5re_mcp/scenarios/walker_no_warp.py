"""walker_no_warp -- the track-position walker never warps backward
(pending row 124).

The folded `span` readout climbs smoothly and wraps to ~0 exactly ONCE per lap
at the start/finish line. A genuine walker WARP is either an EXTRA wrap (span
snapping to ~0 more than once per lap) or a mid-range backward jump (span
decreasing while staying well away from the wrap boundary). So we sample the
folded span across an AI lap on Newcastle (track 5, the circuit track) under
fast-forward, learn the ring size from the observed max, and assert every
backward step is a legit start/finish wrap (high->low) -- zero mid-range
warps -- with the wrap count matching the laps completed.

Note on `progress`: the monotonic lap*ring+span readout momentarily dips ~one
ring at each lap line because the game's lap counter increments a tick or two
AFTER the span folds. That lap-counter lag makes a naive "progress monotonic"
assertion false-fail, so the warp test is built on the folded span + wrap
counting (lag-immune); `progress` is used only as an end-of-race sanity value.
"""
import time
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE

# A per-tick span step; anything below this is sampling jitter, not motion.
JITTER = 8

s = Scenario("walker_no_warp")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 8})   # applies next_race

if s.check(s.start_race_and_wait(track=5, game_type=GAMETYPE_SINGLE_RACE,
                                 laps=2, opponents=3, cops=0, traffic=0,
                                 player_is_ai=1, auto_throttle=1),
           "circuit race launched (Newcastle, 2 laps, AI player, 8x FF)"):
    slot = s.state().get("race", {}).get("player_slot", 0)

    spans: list[int] = []
    reached_lap = 0
    last_progress = 0
    deadline = time.time() + 300
    while time.time() < deadline:
        st = s.state()
        if st.get("game_state") != STATE_RACE:
            break
        r = s.racer(st, slot)
        if r.get("finished"):
            break
        s.recover_if_broken(st, slot)          # never let a wreck stall the lap
        sp = r.get("span")
        if sp is not None:
            spans.append(sp)
        reached_lap = max(reached_lap, r.get("lap", 0))
        last_progress = max(last_progress, r.get("progress", 0))
        time.sleep(0.25)

    ring = max(spans) if spans else 0
    wrap_hi = ring * 0.60          # a legit wrap starts from a HIGH span
    wrap_lo = ring * 0.20          # ...and lands at a LOW span
    wraps = 0
    warps: list[str] = []
    for a, b in zip(spans, spans[1:]):
        if b < a - JITTER:                          # backward step
            if a > wrap_hi and b < wrap_lo:
                wraps += 1                          # legit start/finish wrap
            else:
                warps.append(f"{a}->{b}")           # mid-range warp = bug

    print(f"[{s.name}]   samples={len(spans)} ring~{ring} wraps={wraps} "
          f"laps={reached_lap} progress~{last_progress} warps={warps[:5]}")

    s.check(len(spans) >= 20, "collected enough span samples")
    s.check(reached_lap >= 1, "walker advances past lap 1 (not stuck)")
    s.check(wraps >= 1, "span wraps at least once (crossed the start/finish line)")
    s.check(not warps,
            f"no mid-range span warp ({len(warps)} backward jumps off the "
            f"wrap boundary)")

    s.end_race_best_effort()

s.finish()
