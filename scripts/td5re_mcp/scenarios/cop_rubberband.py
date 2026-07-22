"""cop_rubberband -- cops repeatedly engage a fast moving field, i.e. the
acceleration rubber-band + longer leash keep them on suspects instead of being
dropped (pending_to_test.csv row 154; touches the leash of row 158).

SCOPE / observability: cops are TRAFFIC-slot actors, so their own speed/gap
is NOT in racers[] (racer slots only) -- the precise "out-drags a faster car
on the straight" is not directly observable through get_state. What IS
observable is the suspect racer's `pursued` flag. If the catch-up/leash were
broken, a cop would acquire a fast car once then trip the no-progress
stall-end watchdog and rarely re-engage; a working one keeps cops engaged with
the moving field across the race. This scenario asserts that repeated
engagement (the observable regression guard); the exact per-tick catch-up
would need a cop speed/gap field in get_state (future). Needs the engine's
headless cop recipe: TD5RE_COP_CHASE_AI=1 + an AI-driven field.
"""
import os

os.environ["TD5RE_COP_CHASE_AI"] = "1"        # inherited by the launched game

import sys
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("cop_rubberband")
if not s._owns_proc:
    print("[cop_rubberband] FAIL: needs a fresh launch (TD5RE_COP_CHASE_AI must "
          "be set at process start) -- close the running game first")
    sys.exit(1)


def any_pursued(x):
    return any(r.get("pursued") for r in x.get("race", {}).get("racers", []))


if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=3, cops=1, traffic=4,
                                 player_is_ai=1, auto_throttle=1),
           "cops-on race launched (AI field, COP_CHASE_AI=1)"):
    st = s.wait_until(any_pursued, 120, "a cop acquires a (fast, moving) suspect")
    if s.check(st is not None, "cop acquires a suspect"):
        # What's observable is cop ENGAGEMENT (suspect.pursued), not the cop's
        # own speed. A functioning catch-up/leash keeps cops repeatedly engaged
        # with the moving field; a broken one would acquire once then trip the
        # no-progress stall-end and rarely re-engage. Sample ~15s and require
        # engagement across a meaningful fraction of it (robustly above the
        # acquire-once-then-drop floor; tolerant of pullover/cooldown gaps).
        import time as _t
        hits, samples = 0, 15
        for _ in range(samples):
            _t.sleep(1.0)
            if any_pursued(s.state()):
                hits += 1
        print(f"[{s.name}]   pursued in {hits}/{samples} samples over ~15s")
        # >=3 distinct engaged seconds robustly clears the acquire-once-then-drop
        # floor (~1) while tolerating the intermittent pullover/cooldown cadence.
        s.check(hits >= 3, "cops repeatedly engage the fast field over the race "
                           "(chase AI keeps re-engaging, not acquire-once-drop)")
        s.framedump("chase")

    s.end_race_best_effort()

s.finish()
