"""damage_toggle -- the DAMAGE option gates car damage: ON accumulates damage
on impact, OFF leaves the car pristine (pending row 81).

Two solo races into a wall (human throttle + steer). With car_damage=1 the
player's damage_health must drop / damage_accum rise; with car_damage=0 it
must NOT. Complements crash_fx_human (which only proves the ON case).
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, STATE_RACE, ControlError


def run_into_wall(s, slot, seconds=8.0):
    """Hold throttle + left into a wall; return (start_hp, start_acc, end_hp,
    end_acc) sampled from the player racer."""
    import time as _t
    st0 = s.state(); r0 = s.racer(st0, slot)
    hp0, acc0 = r0.get("damage_health"), r0.get("damage_accum", 0)
    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})
    s.cmd("hold_action", {"slot": slot, "action": "left", "frames": 0})
    end = _t.time() + seconds
    r = r0
    while _t.time() < end:
        _t.sleep(0.5)
        stx = s.state()
        if stx.get("game_state") == STATE_RACE:
            r = s.racer(stx, slot)
    try:
        s.cmd("release_action", {"slot": slot})
    except ControlError:
        pass
    return hp0, acc0, r.get("damage_health"), r.get("damage_accum", 0)


s = Scenario("damage_toggle")

# --- DAMAGE ON: impact must register ---------------------------------------
if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=0, traffic=0, dynamics=0,
                                 player_is_ai=0, auto_throttle=0),
           "race 1 launched (car_damage ON)"):
    s.cmd("set_param", {"name": "car_damage", "value": 1})
    slot = s.state().get("race", {}).get("player_slot", 0)
    hp0, acc0, hp1, acc1 = run_into_wall(s, slot)
    print(f"[{s.name}]   ON:  hp {hp0}->{hp1}  accum {acc0}->{acc1}")
    took = (hp1 is not None and hp0 is not None and hp1 < hp0) or (acc1 > acc0)
    s.check(took, "damage ON: impact reduces health / raises accum")
    s.end_race_best_effort()

# --- DAMAGE OFF: car stays pristine ----------------------------------------
if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=0, traffic=0, dynamics=0,
                                 player_is_ai=0, auto_throttle=0),
           "race 2 launched (car_damage OFF)"):
    s.cmd("set_param", {"name": "car_damage", "value": 0})
    slot = s.state().get("race", {}).get("player_slot", 0)
    hp0, acc0, hp1, acc1 = run_into_wall(s, slot)
    print(f"[{s.name}]   OFF: hp {hp0}->{hp1}  accum {acc0}->{acc1}")
    # With damage off, health must not drop and accum must not climb.
    pristine = ((hp1 is None or hp0 is None or hp1 >= hp0) and acc1 <= acc0)
    s.check(pristine, "damage OFF: no health loss / no accum on impact")
    s.end_race_best_effort()

s.finish()
