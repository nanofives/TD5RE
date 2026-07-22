"""time_trial_midtrack -- a Time Trial on a TD6 city track must NOT instantly
end (pending_to_test.csv line 191).

The bug tripped the start/finish crossing on frame 1 and ended the run before
it began. Assert the race reaches RACE and is STILL running a healthy tick
window after the countdown -- player not finished, game_state still RACE.
Track 26 = Pelton, the first TD6 conversion (TD6 tracks live at 26+).
"""
from _lib import Scenario, GAMETYPE_TIME_TRIAL, STATE_MENU, STATE_RACE

TRACK_TD6_PELTON = 26          # first TD6 conversion (td5_selftest.c race-td6-pelton)

s = Scenario("time_trial_midtrack")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 4})

if s.check(s.start_race_and_wait(track=TRACK_TD6_PELTON, game_type=GAMETYPE_TIME_TRIAL,
                                 laps=1, opponents=0, traffic=0,
                                 player_is_ai=1, auto_throttle=1),
           "TD6 Time Trial reached RACE (mid-track start)"):
    slot = s.state().get("race", {}).get("player_slot", 0)
    start_tick = s.state().get("race", {}).get("sim_tick", 0)

    survived = s.wait_until(
        lambda x: x.get("race", {}).get("sim_tick", 0) > start_tick + 300,
        60, "time trial still running 300+ ticks after start")
    still_racing = (survived is not None
                    and survived.get("game_state") == STATE_RACE
                    and not s.racer(survived, slot).get("finished"))
    if s.check(still_racing, "Time Trial did NOT insta-end at a mid-track start"):
        s.framedump("running")

    s.end_race_best_effort()

s.finish()
