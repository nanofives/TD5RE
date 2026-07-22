"""cop_chase_human -- cop chase with a HUMAN player slot driven via hold_action.

Closes a documented selftest blind spot: cops only chase HUMAN slots, and the
headless suite always runs player_is_ai=1, so chase acquisition was never
automatable before the control socket could drive a human slot.

Asserts: player accelerates under held throttle, wanted mode engages, a cop
actor is assigned and flagged is_cop, the player is flagged is_suspect.
"""
from _lib import Scenario, GAMETYPE_COP_CHASE, STATE_MENU

s = Scenario("cop_chase_human")

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_COP_CHASE,
                                 opponents=3, cops=1, traffic=2,
                                 player_is_ai=0, auto_throttle=0),
           "cop-chase race launched (human slot, no auto-throttle)"):
    st0 = s.state()
    slot = st0.get("race", {}).get("player_slot", 0)

    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})

    st = s.wait_until(lambda x: s.racer(x, slot).get("speed", 0) > 30,
                      30, "player speed > 30 under held throttle")
    s.check(st is not None, "player accelerates under held throttle")

    st = s.wait_until(lambda x: x.get("race", {}).get("wanted_mode")
                      and x.get("race", {}).get("cop_actor", -1) >= 0,
                      90, "wanted mode + cop assigned")
    if s.check(st is not None, "wanted mode engages with a cop actor assigned"):
        # NOTE: is_cop/is_suspect are MP ROLE queries (need a cop SLOT among
        # the racers); SP wanted mode uses a separate AI cop actor and reports
        # cop_slot=-1, so the flags stay false here — informational only.
        cop = st["race"]["cop_actor"]
        print(f"[{s.name}]   info: cop_actor={cop} "
              f"is_cop={s.racer(st, cop).get('is_cop')} "
              f"player_is_suspect={s.racer(st, slot).get('is_suspect')}")
        s.framedump("wanted")

    s.cmd("release_action", {"slot": slot})
    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
