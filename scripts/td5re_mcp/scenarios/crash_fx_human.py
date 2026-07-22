"""crash_fx_human -- damage must accumulate when a human slot hits things.

Closes the UNVALIDATED pending item: headless AI drives too cleanly to crash,
so damage/crash-fx never fired under selftest. Here a human slot holds
throttle plus a steering bias into the walls; damage_health must drop (or
damage_accum rise) from its initial value.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("crash_fx_human")

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=0, traffic=0, dynamics=0,
                                 player_is_ai=0, auto_throttle=0),
           "solo race launched (human slot)"):
    s.cmd("set_param", {"name": "car_damage", "value": 1})
    st0 = s.state()
    slot = st0.get("race", {}).get("player_slot", 0)
    r0 = s.racer(st0, slot)
    hp0 = r0.get("damage_health")
    acc0 = r0.get("damage_accum", 0)
    s.check(hp0 is not None, f"damage system initialized (hp={hp0})")

    # Full throttle with a hard steer: guarantees wall contact within seconds.
    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})
    s.cmd("hold_action", {"slot": slot, "action": "left", "frames": 0})

    def took_damage(x):
        r = s.racer(x, slot)
        if hp0 is not None and r.get("damage_health", hp0) < hp0:
            return True
        return r.get("damage_accum", acc0) > acc0

    st = s.wait_until(took_damage, 60, "damage registered from wall contact")
    if s.check(st is not None, "damage accumulates on impact"):
        r = s.racer(st, slot)
        print(f"[{s.name}]   hp {hp0} -> {r.get('damage_health')}, "
              f"accum {acc0} -> {r.get('damage_accum')}")
        s.framedump("damaged")

    s.cmd("release_action", {"slot": slot})
    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30, "back at MENU")

s.finish()
