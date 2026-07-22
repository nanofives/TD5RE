"""traffic_battle_solo -- solo Traffic Battle via TD5RE_BATTLE=1 has the right
composition: battle mode active, no rival racers, no cops, traffic present
(pending_to_test.csv rows 168 / 170).

TD5RE_BATTLE=1 (read at InitRace in td5_game.c) forces
mp_mode_config.mode = TRAFFIC_BATTLE for a single-player race. Asserts the
race.battle flag (new get_state field = td5_game_battle_mode_active), a field
of just the player (no AI rivals), no cop actor, and traffic actors spawned.
"""
import os

os.environ["TD5RE_BATTLE"] = "1"              # inherited by the launched game

import sys
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("traffic_battle_solo")
if not s._owns_proc:
    print("[traffic_battle_solo] FAIL: needs a fresh launch (TD5RE_BATTLE must "
          "be set at process start) -- close the running game first")
    sys.exit(1)

# opponents=0 (no rivals), cops=0, traffic on: the battle-mode composition.
if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=0, cops=0, traffic=3,
                                 player_is_ai=1, auto_throttle=1),
           "solo battle race launched (TD5RE_BATTLE=1, opponents=0)"):
    st = s.state()
    race = st.get("race", {})
    s.check(race.get("battle", False), "Traffic Battle mode is active")
    s.check(race.get("num_racers", 99) == 1, "no rival racers (player only)")
    s.check(race.get("cop_actor", 0) < 0, "no cops in Traffic Battle")
    # Traffic actors live above the racer slots; battle spawns oncoming traffic.
    st2 = s.wait_until(lambda x: x.get("race", {}).get("num_actors", 0)
                       > x.get("race", {}).get("num_racers", 0),
                       30, "traffic actors spawn")
    s.check(st2 is not None, "oncoming traffic present (num_actors > num_racers)")
    if st2 is not None:
        s.framedump("battle")

    s.end_race_best_effort()

s.finish()
