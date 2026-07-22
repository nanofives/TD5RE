"""drag_natural_finish -- a drag race must END NATURALLY under real throttle.

Closes the KNOWN selftest issue (finding 2026-07-02): a natural drag finish
never fires under AutoRace/auto-throttle, so the suite runs drag on a tick
budget only. Here a human slot holds real throttle; the race must end
WITHOUT any end_race command.

Asserts: player accelerates, the player slot reaches finished=1 (or the game
leaves RACE state on its own), and a finish position was recorded.
"""
from _lib import Scenario, GAMETYPE_DRAG_RACE, STATE_RACE, ControlError

s = Scenario("drag_natural_finish")

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_DRAG_RACE,
                                 opponents=1, player_is_ai=0, auto_throttle=0),
           "drag race launched (human slot, no auto-throttle)"):
    st0 = s.state()
    slot = st0.get("race", {}).get("player_slot", 0)

    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})

    st = s.wait_until(lambda x: s.racer(x, slot).get("speed", 0) > 20,
                      30, "player moving under held throttle")
    s.check(st is not None, "player accelerates off the line")

    # Natural finish: player crosses the line (finished=1) or the game flow
    # itself leaves RACE. NO end_race is ever sent in this scenario.
    st = s.wait_until(lambda x: x.get("game_state") != STATE_RACE
                      or s.racer(x, slot).get("finished"),
                      180, "natural drag finish", recover_slot=slot)
    if s.check(st is not None, "drag race ends naturally (no end_race sent)"):
        r = s.racer(st, slot)
        if st.get("game_state") == STATE_RACE:
            s.check(r.get("finish_position", -1) >= 0,
                    f"finish position recorded ({r.get('finish_position')})")
            s.framedump("finished")

    # Pure cleanup after the natural finish — the process may already be
    # tearing down through results, so don't let a dropped socket traceback.
    try:
        s.cmd("release_action", {"slot": slot})
    except ControlError:
        pass

s.finish()
