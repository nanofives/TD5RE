"""heavy_climb -- heavier cars lose more speed climbing hills (pending row 247).

The `climb` readout (signed per-tick vertical rate, >0 = uphill) makes "am I on
an uphill" observable; `heaviness` (Q8) tells the cars apart. Two solo-ish AI
runs on Courmayeur (track 7, the alpine/hilly track): for each car, sample
`speed` split by climb>0 (uphill) vs climb~0 (flat), and form the uphill/flat
speed ratio -- which normalises out raw top-speed differences so it isolates
the climb penalty. The HEAVIER car must retain LESS speed uphill relative to
its flat pace (ratio_heavy < ratio_light).

Medium confidence: AI-line and terrain noise. If it proves flaky the `climb`
readout still lands and 247 stays blocked.
"""
import time
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE

TRACK_COURMAYEUR = 7
FLAT_BAND = 200          # |climb| <= this (24.8 units/tick) counts as flat
SAMPLE_SECS = 40         # wall seconds of sampling per run (under 8x FF)


def profile(s, car):
    """Solo AI run on the hilly track; return (heaviness, uphill_avg, flat_avg)
    speeds, or (None, None, None) on launch failure."""
    if not s.start_race_and_wait(track=TRACK_COURMAYEUR,
                                 game_type=GAMETYPE_SINGLE_RACE, car=car,
                                 laps=2, opponents=1, cops=0, traffic=0,
                                 player_is_ai=1, auto_throttle=1):
        return None, None, None
    slot = s.state().get("race", {}).get("player_slot", 0)
    heaviness = s.racer(s.state(), slot).get("heaviness", 0)
    up_sum = up_n = flat_sum = flat_n = 0
    deadline = time.time() + SAMPLE_SECS
    while time.time() < deadline:
        st = s.state()
        if st.get("game_state") != STATE_RACE:
            break
        r = s.racer(st, slot)
        if r.get("finished"):
            break
        s.recover_if_broken(st, slot)          # a wreck would skew the samples
        spd = r.get("speed", 0)
        climb = r.get("climb", 0)
        if spd > 0:
            if climb > FLAT_BAND:
                up_sum += spd; up_n += 1
            elif abs(climb) <= FLAT_BAND:
                flat_sum += spd; flat_n += 1
        time.sleep(0.2)
    s.end_race_best_effort()
    up_avg = (up_sum / up_n) if up_n else None
    flat_avg = (flat_sum / flat_n) if flat_n else None
    return heaviness, up_avg, flat_avg


s = Scenario("heavy_climb")

h0, up0, flat0 = profile(s, 0)
h8, up8, flat8 = profile(s, 8)
print(f"[{s.name}]   car0: heaviness={h0} uphill_avg={up0} flat_avg={flat0}")
print(f"[{s.name}]   car8: heaviness={h8} uphill_avg={up8} flat_avg={flat8}")

if s.check(None not in (up0, flat0, up8, flat8) and flat0 and flat8,
           "both cars produced uphill + flat speed samples"):
    if s.check(h0 != h8, "the two cars differ in heaviness"):
        r0 = up0 / flat0
        r8 = up8 / flat8
        heavy_ratio = r0 if h0 > h8 else r8
        light_ratio = r8 if h0 > h8 else r0
        print(f"[{s.name}]   uphill/flat ratio: heavy={heavy_ratio:.3f} "
              f"light={light_ratio:.3f}")
        s.check(heavy_ratio < light_ratio,
                f"heavier car retains less speed uphill "
                f"(ratio {heavy_ratio:.3f} < {light_ratio:.3f})")

s.finish()
