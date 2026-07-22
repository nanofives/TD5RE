"""cop_leash -- the longer cop chase leash (110 spans) makes a cop harder to
shake than a short leash (pending row 160 / "longer chase leash 110 spans").

A cop abandons a chase when the target escapes beyond cop_chase_max_gap()
spans (TD5RE_COP_CHASE_GAP, default 110), logging `cop_chase END (too far):
... gap=N` to race.log. With a SHORT leash the cop gives up easily (frequent
too-far ends, at small gaps); with the long default it stays on the target
(fewer too-far ends). Two launches (env is read once at chase-setup) compare
the give-up behaviour; the cop recipe is TD5RE_COP_CHASE_AI=1 + an AI field.
"""
import os
import re
import sys
import time

os.environ["TD5RE_COP_CHASE_AI"] = "1"       # inherited by the launched game

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE, REPO_ROOT
import launcher

TOOFAR = re.compile(r"cop_chase END \(too far\): cop=\d+ target=\d+ gap=(-?\d+)")


def measure(leash: int):
    """Launch with a cop-chase leash, run once a chase is seen, return
    (chase_started, too_far_count, max_gap, pursued_frac) + the Scenario."""
    os.environ["TD5RE_COP_CHASE_GAP"] = str(leash)
    s = Scenario(f"cop_leash_G{leash}")
    if not s._owns_proc:
        print(f"[cop_leash] FAIL: leash {leash} needs a fresh launch "
              "(env fixed at spawn) -- close the running game first")
        s.client.close()
        return (False, 0, 0, 0.0), s

    started = False
    pursued_hits = pursued_n = 0
    s.cmd("set_param", {"name": "trace_fast_forward", "value": 8})
    if s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                             opponents=3, cops=1, traffic=4,
                             player_is_ai=1, auto_throttle=1):
        def any_pursued(x):
            return any(r.get("pursued") for r in x.get("race", {}).get("racers", []))
        st = s.wait_until(any_pursued, 120, "a cop acquires a suspect", recover_slot=0)
        started = st is not None
        if started:
            end = time.time() + 25
            while time.time() < end:
                cur = s.state()
                if cur.get("game_state") != STATE_RACE:
                    break
                s.recover_if_broken(cur, 0)
                pursued_n += 1
                if any_pursued(cur):
                    pursued_hits += 1
                time.sleep(0.5)

    # Parse THIS launch's race.log (truncated per launch) before teardown.
    too_far = []
    try:
        text = (REPO_ROOT / "log" / "race.log").read_text(errors="ignore")
        too_far = [abs(int(m.group(1))) for m in TOOFAR.finditer(text)]
    except OSError:
        pass
    launcher.stop(s.proc, client=s.client)
    s.client.close()
    frac = (pursued_hits / pursued_n) if pursued_n else 0.0
    return (started, len(too_far), max(too_far, default=0), frac), s


(short_started, short_tf, short_gap, short_frac), s = measure(30)     # short leash
(long_started,  long_tf,  long_gap,  long_frac), _ = measure(110)     # long default

s.checks = []
print(f"[cop_leash]   SHORT(30):  started={short_started} too_far_ends={short_tf} "
      f"max_gap={short_gap} pursued_frac={short_frac:.2f}")
print(f"[cop_leash]   LONG(110):  started={long_started} too_far_ends={long_tf} "
      f"max_gap={long_gap} pursued_frac={long_frac:.2f}")

if s.check(short_started and long_started, "a cop chase started under both leashes"):
    # "Harder to shake" with the long leash: it abandons the target LESS often
    # (fewer too-far give-ups) and/or stays engaged a larger fraction of the time.
    s.check(long_tf <= short_tf and (long_frac >= short_frac or short_tf > long_tf),
            f"long leash is harder to shake (too-far ends {long_tf} <= {short_tf}; "
            f"pursued_frac {long_frac:.2f} vs {short_frac:.2f})")
    # When the short leash DOES give up, it does so at a small gap (~its leash),
    # well under the long leash's 110-span reach.
    if short_gap:
        s.check(short_gap <= 90,
                f"short-leash give-up gap stays small ({short_gap} <= 90 spans)")

s.proc = None
s._owns_proc = False
s.finish()
