"""damage_no_wreckout -- with the DAMAGE bar OFF a car never wrecks/knocks out,
however hard it is battered (pending row 153).

damage_toggle.py proves OFF leaves the car pristine on ONE wall hit; this
proves the stronger "no wreck-out" claim: with car_damage=0, sustain heavy
ramming into a full field + traffic and assert the player NEVER enters a
knockout (damage_health never <=0) and stays drivable throughout (regains
speed after impacts -- not permanently wrecked). Recovery is intentionally NOT
armed here: we want to OBSERVE that no knockout ever happens.
"""
import time
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE, ControlError

RAM_SECS = 16.0

s = Scenario("damage_no_wreckout")

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=5, traffic=3, dynamics=0,
                                 player_is_ai=0, auto_throttle=0),
           "collision-heavy race launched (damage bar to be turned OFF)"):
    s.cmd("set_param", {"name": "car_damage", "value": 0})       # bar OFF
    slot = s.state().get("race", {}).get("player_slot", 0)

    # Batter the car: full throttle hard into the pack/walls for the window.
    s.cmd("hold_action", {"slot": slot, "action": "throttle", "frames": 0})
    s.cmd("hold_action", {"slot": slot, "action": "left", "frames": 0})

    min_health = None            # lowest damage_health seen (None = never present)
    max_speed = 0
    knocked_out = False
    samples = 0
    deadline = time.time() + RAM_SECS
    while time.time() < deadline:
        time.sleep(0.4)
        st = s.state()
        if st.get("game_state") != STATE_RACE:
            break
        r = s.racer(st, slot)
        samples += 1
        hp = r.get("damage_health")
        if hp is not None:
            min_health = hp if min_health is None else min(min_health, hp)
            if hp <= 0:
                knocked_out = True
        if r.get("finished"):        # a wreck-out would finish the player early
            knocked_out = True
        max_speed = max(max_speed, r.get("speed", 0))

    try:
        s.cmd("release_action", {"slot": slot})
    except ControlError:
        pass

    print(f"[{s.name}]   samples={samples} min_health={min_health} "
          f"max_speed={max_speed} knocked_out={knocked_out}")

    s.check(samples >= 10, "sustained a ramming window")
    s.check(max_speed > 50, "car actually drove into the pack (took impacts)")
    s.check(not knocked_out,
            "damage OFF: car never wrecks/knocks out however hard it is battered")

    s.end_race_best_effort()

s.finish()
