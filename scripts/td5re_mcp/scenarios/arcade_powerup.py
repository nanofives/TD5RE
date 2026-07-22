"""arcade_powerup -- an arcade race must actually hand out a power-up.

Selftest runs arcade-with-traffic-and-cops but has no way to observe whether
any racer ever ACQUIRES a power-up effect. With the racers[] arcade_effect /
arcade_frames fields this becomes assertable: dynamics=0 (ARCADE — 1 is SIMULATION, see td5_arcade.c init),
wait until any racer's arcade_effect != TD5_PU_NONE (0), then watch its
frame timer count down.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("arcade_powerup")


def slot_with_effect(st):
    for r in st.get("race", {}).get("racers", []):
        if r.get("arcade_effect", 0) > 0:
            return r
    return None


if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=3, traffic=2, dynamics=0,
                                 player_is_ai=1, auto_throttle=1),
           "arcade race launched (dynamics=0 = ARCADE)"):
    st = s.state()
    if s.check(st.get("race", {}).get("arcade_active", False),
               "arcade mode reports active"):
        st = s.wait_until(lambda x: slot_with_effect(x) is not None,
                          120, "any racer picks up a power-up", recover_slot=0)
        if s.check(st is not None, "a power-up effect was acquired"):
            r = slot_with_effect(st)
            slot, effect = r["slot"], r["arcade_effect"]
            frames0 = r.get("arcade_frames", 0)
            s.check(frames0 > 0, f"effect {effect} on slot {slot} has a timer "
                                 f"({frames0} frames)")
            s.framedump("powerup")

            def timer_moved(x):
                rr = s.racer(x, slot)
                # effect either ticked down or already expired/replaced
                return rr.get("arcade_effect", 0) != effect \
                    or rr.get("arcade_frames", frames0) < frames0
            st = s.wait_until(timer_moved, 30, "effect timer counts down")
            s.check(st is not None, "effect timer counts down (or expires)")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
