"""cops_trigger_speeding -- speeding past a cruising cop engages wanted mode.

[WORKER DRAFT 2026-07-21, pending_to_test.csv row 29 — review + move up one
directory to include in run_all.py once validated.]

Row 29: the cop ACQUIRE gap window was widened ([0,15]->[-8,60] spans) and the
acquire speed relaxed to 85% of the speeding threshold; a human driving fast
should now reliably trigger a chase. Asserts wanted mode engages and a cop
actor is assigned under sustained throttle (human slot, no auto-throttle).
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("cops_trigger_speeding")

if s.check(s.start_race_and_wait(track=5, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=3, cops=1, traffic=1,
                                 player_is_ai=0, auto_throttle=0),
           "cops-on race launched (human slot, no auto-throttle)"):
    slot = s.state().get("race", {}).get("player_slot", 0)
    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})

    st = s.wait_until(lambda x: s.racer(x, slot).get("speed", 0) > 40,
                      30, "player speeding (>40) under held throttle")
    s.check(st is not None, "player reaches speeding pace")

    st = s.wait_until(lambda x: x.get("race", {}).get("wanted_mode")
                      and x.get("race", {}).get("cop_actor", -1) >= 0,
                      90, "wanted mode + cop assigned while speeding")
    if s.check(st is not None, "speeding past a cop triggers a chase"):
        s.framedump("wanted")

    s.cmd("release_action", {"slot": slot})
    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
