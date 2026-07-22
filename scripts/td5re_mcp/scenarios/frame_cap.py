"""frame_cap -- TD5RE_FRAME_CAP limits the render rate when VSync is off
(pending row 19).

Measures the ACTUAL present rate via get_state.present_count (sampled over a
wall interval) -- not race.log's fps, which is a sim-timing metric. Launches
VSync-off with a low cap and asserts the measured present rate sits near/below
it (far under the uncapped/144 default).
"""
import os
import time

CAP = 40
os.environ["TD5RE_FRAME_CAP"] = str(CAP)      # inherited by the launched game

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

s = Scenario("frame_cap", extra_args="--VSync=0")


def present_rate(seconds=3.0):
    p0 = s.state().get("present_count")
    t0 = time.time()
    time.sleep(seconds)
    p1 = s.state().get("present_count")
    dt = time.time() - t0
    if p0 is None or p1 is None or dt <= 0:
        return None
    return (p1 - p0) / dt


if s.check(s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30,
                        "frontend") is not None, "frontend reached"):
    rate = present_rate(3.0)
    print(f"[{s.name}]   present rate ~= {rate:.0f}/s (cap {CAP})"
          if rate is not None else "no present samples")
    if s.check(rate is not None, "measured a present rate"):
        # Capped near CAP; allow generous slack for pacing granularity, but it
        # must be far below the uncapped/144-default rate (~170+ seen uncapped).
        s.check(rate <= CAP * 1.6,
                f"render rate honours the cap (~{rate:.0f}/s <= {CAP*1.6:.0f}, "
                f"not the uncapped ~170/s)")

s.finish()
