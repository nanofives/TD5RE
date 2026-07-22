"""_selftest_crash -- harness self-test: force a crash mid-race and confirm
_lib reports CRASH (exit 3) with a forensics snapshot, not a silent pass or a
bare traceback. Underscore-prefixed so run_all does NOT auto-discover it; run
explicitly:  python scripts/td5re_mcp/scenarios/_selftest_crash.py
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE

s = Scenario("_selftest_crash")
s.check(s.start_race_and_wait(track=17, game_type=GAMETYPE_SINGLE_RACE,
                              opponents=2, player_is_ai=1, auto_throttle=1),
        "race launched")
# This command never returns a reply — the game faults on the main thread.
# _lib.cmd() must detect the crash and exit(CRASH_EXIT=3).
s.cmd("force_crash")
# Should be unreachable:
s.check(False, "reached code after force_crash (crash NOT captured)")
s.finish()
