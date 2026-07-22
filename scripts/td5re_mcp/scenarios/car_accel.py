"""car_accel -- the power-to-weight ACCEL stat predicts real acceleration
(pending row 248).

Two solo-ish DRAG runs (the player slot is always active and drives a straight
line, so speed is pure acceleration -- no corners, no drag-lane field-size
issues). For two different cars, read the player's `accel` stat and measure
how many sim ticks it takes to reach a target speed. The higher-accel car must
reach the target in FEWER ticks. Validates the stat <-> physics link.

(247 "heavy cars climb hills slower" still needs a known uphill segment /
gradient readout to isolate the climb -- auto-blocked despite heaviness being
exposed.)
"""
from _lib import Scenario, GAMETYPE_DRAG_RACE, STATE_MENU, STATE_RACE

TARGET_SPEED = 300


def measure(s, car):
    """Launch a drag run with the player driving `car`; return (accel_stat,
    ticks_to_TARGET or None)."""
    if not s.start_race_and_wait(track=0, game_type=GAMETYPE_DRAG_RACE,
                                 car=car, opponents=1,
                                 player_is_ai=1, auto_throttle=1):
        return None, None
    slot = s.state().get("race", {}).get("player_slot", 0)
    accel = s.racer(s.state(), slot).get("accel", 0)
    start_tick = s.state().get("race", {}).get("sim_tick", 0)
    st = s.wait_until(lambda x: s.racer(x, slot).get("speed", 0) >= TARGET_SPEED,
                      40, f"car {car} reaches {TARGET_SPEED}", recover_slot=slot)
    ticks = None
    if st is not None:
        ticks = st.get("race", {}).get("sim_tick", 0) - start_tick
    s.end_race_best_effort()
    return accel, ticks


s = Scenario("car_accel")

# Two distinct car indices; read their accel stats live and compare accel time.
a_accel, a_ticks = measure(s, 0)
b_accel, b_ticks = measure(s, 8)
print(f"[{s.name}]   car0: accel={a_accel} ticks_to_{TARGET_SPEED}={a_ticks} | "
      f"car8: accel={b_accel} ticks_to_{TARGET_SPEED}={b_ticks}")

if s.check(a_ticks is not None and b_ticks is not None,
           "both cars reached the target speed on the strip"):
    if s.check(a_accel != b_accel, "the two cars differ in power-to-weight"):
        # Higher accel stat -> reaches target in fewer ticks.
        hi_ticks = a_ticks if a_accel > b_accel else b_ticks
        lo_ticks = b_ticks if a_accel > b_accel else a_ticks
        s.check(hi_ticks <= lo_ticks,
                f"higher-accel car accelerates faster "
                f"({hi_ticks} <= {lo_ticks} ticks to {TARGET_SPEED})")

s.finish()
