"""tutorial_every_race -- the controller tutorial overlay re-arms at the START
of EVERY race, not just the very first one ever (pending row 140).

tutorial_keyboard.py proves the overlay shows for race #1. This proves the
"once-ever -> every race" change: run several human-slot races back to back
and assert race.tutorial arms at the start of EACH, dismissing it (ENTER)
between races. A regression to once-ever would arm only the first.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, STATE_RACE

N_RACES = 3

s = Scenario("tutorial_every_race")

armed = 0
for i in range(1, N_RACES + 1):
    if not s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30,
                        f"MENU before race {i}"):
        break
    # Human keyboard slot; do NOT use start_race_and_wait (it auto-dismisses the
    # overlay) -- we want to observe the arm on each race.
    res = s.cmd("start_race", {"track": 0, "game_type": GAMETYPE_SINGLE_RACE,
                               "opponents": 3, "player_is_ai": 0,
                               "auto_throttle": 0})
    if not res.get("ok"):
        s.check(False, f"race {i} launched")
        break
    st = s.wait_until(lambda x: x.get("game_state") == STATE_RACE
                      and x.get("race", {}).get("tutorial"),
                      40, f"tutorial overlay arms at start of race {i}")
    hit = st is not None
    s.check(hit, f"tutorial overlay shows at the start of race {i}")
    if hit:
        armed += 1
        s.cmd("tap_key", {"dik": 0x1C})          # ENTER dismisses the overlay
        s.wait_until(lambda x: not x.get("race", {}).get("tutorial", True),
                     15, f"overlay dismissed for race {i}")
    s.end_race_best_effort()

print(f"[{s.name}]   overlay armed in {armed}/{N_RACES} consecutive races")
s.check(armed == N_RACES,
        f"tutorial re-arms every race (not once-ever): {armed}/{N_RACES}")

s.finish()
