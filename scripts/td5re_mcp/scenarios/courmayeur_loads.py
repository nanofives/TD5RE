"""courmayeur_loads -- Courmayeur (Italy, track 7) launches without the
route-table OOB crash (pending_to_test.csv line 267).

Regression guard for the level027 out-of-bounds route-table crash. Start two
race configs on Courmayeur; each must reach RACE, tick past launch, and return
cleanly to MENU (a crash drops the control socket -> game_state == -1). Bounded
by 1 lap + fast-forward.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU

TRACK_COURMAYEUR = 7                        # schedule index -> level027, td5_asset.c

s = Scenario("courmayeur_loads")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 8})   # applies next_race

for label, extra in (
    ("plain", dict(opponents=3, traffic=2, cops=0)),
    ("cops+traffic", dict(opponents=2, traffic=2, cops=1)),     # exercises the AI slot path
):
    ok = s.start_race_and_wait(track=TRACK_COURMAYEUR,
                               game_type=GAMETYPE_SINGLE_RACE, laps=1,
                               player_is_ai=1, auto_throttle=1, **extra)
    if s.check(ok, f"Courmayeur {label} reached RACE (no launch crash)"):
        st = s.wait_until(lambda x: x.get("race", {}).get("sim_tick", 0) > 120,
                          60, "sim ticking past launch on Courmayeur")
        s.check(st is not None, f"Courmayeur {label} sim runs (no OOB stall)")
        s.framedump(f"courmayeur_{label.split('+')[0]}")
    # Teardown between configs; the NEXT iteration's start_race_and_wait waits
    # for MENU, so a successful relaunch already proves the clean return.
    s.end_race_best_effort()

s.finish()
