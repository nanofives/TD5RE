"""split_drag -- a split-screen Drag Race launches the drag strip cleanly and
doesn't crash on the wrong track (pending row 54).

The bug: split drag loaded a wrong/again track and could crash. Assert a
2-player drag race reaches RACE in drag mode with 2 racer slots (split, no AI
field) and the sim runs — i.e. it launched the drag strip without crashing.
"""
from _lib import Scenario, GAMETYPE_DRAG_RACE, STATE_MENU

s = Scenario("split_drag")

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_DRAG_RACE,
                                 players=2, opponents=0,
                                 player_is_ai=1, auto_throttle=1),
           "2-player drag race reached RACE (no wrong-track crash)"):
    st = s.state()
    race = st.get("race", {})
    s.check(race.get("game_type") == GAMETYPE_DRAG_RACE, "drag game mode active")
    s.check(race.get("num_racers", 0) == 2, "two racer slots (split, no AI field)")
    st2 = s.wait_until(lambda x: x.get("race", {}).get("sim_tick", 0) > 60,
                       30, "sim ticking on the drag strip")
    s.check(st2 is not None, "drag sim runs (launched the strip, no stall/crash)")
    s.framedump("split_drag")
    s.end_race_best_effort()

s.finish()
