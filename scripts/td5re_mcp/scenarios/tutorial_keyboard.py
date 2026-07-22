"""tutorial_keyboard -- the first-race controller tutorial overlay shows for a
KEYBOARD (human, non-pad) player, holding the countdown until dismissed
(pending row 83).

A human slot (player_is_ai=0) with the tutorial option on must raise
race.tutorial once the race is entered; the game stays paused (countdown
held) until a key is pressed. Asserts the overlay arms for a keyboard human,
then dismisses it (ENTER) and confirms the race proceeds.
"""
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, STATE_RACE

s = Scenario("tutorial_keyboard")

if s.check(s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30,
                        "MENU state") is not None, "frontend reached"):
    # Human keyboard slot; do NOT use start_race_and_wait (it auto-dismisses
    # the tutorial) — we want to observe the overlay arm first.
    res = s.cmd("start_race", {"track": 0, "game_type": GAMETYPE_SINGLE_RACE,
                               "opponents": 3, "player_is_ai": 0,
                               "auto_throttle": 0})
    if s.check(res.get("ok"), "keyboard-player race launched"):
        st = s.wait_until(lambda x: x.get("game_state") == STATE_RACE
                          and x.get("race", {}).get("tutorial"),
                          40, "tutorial overlay arms for the keyboard player")
        if s.check(st is not None, "tutorial overlay shows on a keyboard human"):
            s.framedump("tutorial")
            # Countdown is held while the overlay is up.
            s.check(st.get("race", {}).get("countdown", False),
                    "countdown held while tutorial overlay is up")
            # Dismiss with ENTER; the overlay clears and the race proceeds.
            s.cmd("tap_key", {"dik": 0x1C})
            done = s.wait_until(lambda x: not x.get("race", {}).get("tutorial", True),
                                15, "overlay dismissed after keypress")
            s.check(done is not None, "ENTER dismisses the tutorial overlay")
        s.end_race_best_effort()

s.finish()
