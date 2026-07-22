"""arcade_only_sim -- power-up boxes are ARCADE-only: a SIMULATION race must
never hand out a power-up (pending_to_test.csv line 183).

Complements arcade_powerup.py (which proves ARCADE grants effects). Here
dynamics=1 (SIMULATION -- 0 is ARCADE; td5_arcade.c gates on ==0): assert
arcade_active is false and NO racer's arcade_effect ever goes non-zero across
a bounded, fast-forwarded window with a full AI field + traffic.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, STATE_RACE

s = Scenario("arcade_only_sim")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 6})


def any_effect(st):
    return any(r.get("arcade_effect", 0) > 0
               for r in st.get("race", {}).get("racers", []))


if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 laps=1, opponents=3, traffic=2, dynamics=1,
                                 player_is_ai=1, auto_throttle=1),
           "SIMULATION race launched (dynamics=1 = SIM)"):
    st0 = s.state()
    s.check(not st0.get("race", {}).get("arcade_active", False),
            "arcade mode reports INACTIVE under SIMULATION")

    start_tick = st0.get("race", {}).get("sim_tick", 0)
    # Bounded on sim_tick (FF-safe); trip early if any effect ever leaks.
    reached = s.wait_until(
        lambda x: any_effect(x)
        or x.get("game_state") != STATE_RACE
        or x.get("race", {}).get("sim_tick", 0) > start_tick + 600,
        90, "SIM observation window elapsed (or a power-up leaked)",
        recover_slot=0)          # keep the field traversing the box lanes
    saw_effect = reached is not None and any_effect(reached)
    if saw_effect:
        s.framedump("leaked_powerup")
    s.check(not saw_effect,
            "no power-up effect ever spawns in SIMULATION (arcade-only)")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
