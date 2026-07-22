"""reverse_circuit_laps -- reverse-direction circuit completes its lap count.

pending_to_test.csv row 188: reverse now works on the 7-lap circuits
(checkpoints, lap counting, finish detection, AI routing). Drives an AI
player (auto_throttle) on Newcastle (track 5, the circuit track) in REVERSE
under fast-forward, laps pinned to 2 to bound runtime, and asserts the lap
counter advances past lap 1 and the race reaches a natural finish -- with no
span warp / stuck walker.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, STATE_RACE

s = Scenario("reverse_circuit_laps")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 8})   # applies next_race

if s.check(s.start_race_and_wait(track=5, game_type=GAMETYPE_SINGLE_RACE,
                                 reverse=1, laps=2, opponents=3, cops=0,
                                 traffic=0, player_is_ai=1, auto_throttle=1),
           "reverse circuit race launched (Newcastle, 2 laps, AI player, 8x FF)"):
    slot = s.state().get("race", {}).get("player_slot", 0)

    st = s.wait_until(lambda x: s.racer(x, slot).get("lap", 0) >= 1,
                      120, "player lap counter advances (reverse routing OK)")
    s.check(st is not None, "lap counter increments in reverse")

    # Natural finish: the player crosses the line or the game leaves RACE on
    # its own. NO end_race is sent before this point.
    st = s.wait_until(lambda x: x.get("game_state") != STATE_RACE
                      or s.racer(x, slot).get("finished"),
                      300, "race reaches a natural finish (full lap count)")
    if s.check(st is not None, "reverse race finishes without warp/stall"):
        if st.get("game_state") == STATE_RACE:
            s.framedump("finish")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
