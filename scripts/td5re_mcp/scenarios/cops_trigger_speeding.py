"""cops_trigger_speeding -- speeding past a traffic cop triggers a pursuit.

pending_to_test.csv row 29: cops acquire a racer that passes them (gap window
0..60 spans) above the speed gate (CopMinSpeed 0x15638 raw * 85%).

Observable via racers[].pursued (td5_ai_actor_is_pursued) — NOT wanted_mode/
cop_actor, which belong to the dedicated COP_CHASE mode (learned run 1).

Driving: a HUMAN slot can hit the gate speed but wrecks in traffic before it
ever passes an IDLE cop (diag runs: stuck at span 282, spd 0 vs gate 74466).
The engine's own headless recipe is TD5RE_COP_CHASE_AI=1 (cops may acquire
AI-driven racers, td5_ai.c cop_chase_ai_debug) + an AI-driven field — the AI
sustains racing speed and survives traffic. Env must be set BEFORE launch;
this scenario therefore requires launching its own game instance.
"""
import os
import sys

os.environ["TD5RE_COP_CHASE_AI"] = "1"       # inherited by the launched game

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("cops_trigger_speeding")
if not s._owns_proc:
    print("[cops_trigger_speeding] FAIL: needs a fresh launch "
          "(TD5RE_COP_CHASE_AI must be set at process start) — close the "
          "running game first")
    sys.exit(1)


def any_pursued(x):
    return any(r.get("pursued") for r in x.get("race", {}).get("racers", []))


if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=3, cops=1, traffic=4,
                                 player_is_ai=1, auto_throttle=1),
           "cops-on race launched (Moscow, AI-driven field, COP_CHASE_AI=1)"):
    st = s.wait_until(lambda x: max((r.get("speed", 0) for r in
                                     x.get("race", {}).get("racers", [])),
                                    default=0) > 300,
                      60, "some racer reaches chase-triggering pace (>300)")
    s.check(st is not None, "field reaches chase-triggering pace")

    st = s.wait_until(any_pursued, 180,
                      "a traffic cop starts pursuing a speeding racer")
    if s.check(st is not None, "speeding past a cop triggers a pursuit"):
        chased = [r["slot"] for r in st["race"]["racers"] if r.get("pursued")]
        print(f"[{s.name}]   info: pursued racer slot(s): {chased}")
        s.framedump("pursued")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 45, "back at MENU")

s.finish()
