"""reverse_circuit_laps -- reverse-direction race completes its full lap count.

[WORKER DRAFT 2026-07-21, pending_to_test.csv row 188 — review + move up one
directory to include in run_all.py once validated. track= is a PLACEHOLDER:
set it to a real 7-lap circuit index (e.g. Newcastle) before promoting.]

Row 188: reverse now works on the 7-lap circuits (checkpoints, lap counting,
finish detection, AI routing). Drives an AI player (auto_throttle) under
fast-forward and asserts the lap counter advances past lap 1 and the race
reaches a finish -- with no span warp / stuck walker.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("reverse_circuit_laps")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 4})   # applies next_race

if s.check(s.start_race_and_wait(track=5, game_type=GAMETYPE_SINGLE_RACE,
                                 reverse=1, opponents=3, cops=0, traffic=0,
                                 player_is_ai=1, auto_throttle=1),
           "reverse circuit race launched"):
    slot = s.state().get("race", {}).get("player_slot", 0)

    st = s.wait_until(lambda x: s.racer(x, slot).get("lap", 0) >= 2,
                      120, "player lap advances past lap 1 (reverse routing OK)")
    s.check(st is not None, "lap counter increments in reverse")

    st = s.wait_until(lambda x: s.racer(x, slot).get("finished")
                      or x.get("race", {}).get("victory_position", 0) > 0,
                      600, "race reaches a finish (full lap count)")
    if s.check(st is not None, "reverse race finishes without warp/crash"):
        s.framedump("finish")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
