"""inputscript_drive -- the [Trace] InputScript harness actually drives the
car from a script file (pending row 111).

Writes a tiny script (dismiss the tutorial, then hold throttle), launches with
--InputScript=<file>, and asserts the player accelerates with NO control-socket
input from the scenario -- i.e. the scripted input alone moved the car. This
exercises the real InputScript path (td5_inputscript_race_bits), the sibling of
the live-control hold_action overlay.
"""
from pathlib import Path
from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, REPO_ROOT

# Write the script BEFORE launching. Path is relative to the game's CWD (repo
# root). Dismiss the first-race tutorial (ENTER) at race entry, then hold
# throttle a beat later so the countdown has cleared.
SCRIPT_REL = "log/scenarios/inputscript_drive.ist"
(REPO_ROOT / "log" / "scenarios").mkdir(parents=True, exist_ok=True)
(REPO_ROOT / SCRIPT_REL).write_text(
    "sync race\n"
    "key enter press\n"
    "+120 throttle 1\n"
)

s = Scenario("inputscript_drive", extra_args=f"--InputScript={SCRIPT_REL}")

# Human slot, no auto-throttle: only the InputScript can move the car.
if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 opponents=3, player_is_ai=0, auto_throttle=0),
           "race launched with an InputScript"):
    slot = s.state().get("race", {}).get("player_slot", 0)
    # No hold_action from us — if the car moves, the script drove it.
    st = s.wait_until(lambda x: s.racer(x, slot).get("speed", 0) > 20,
                      40, "player accelerates under scripted throttle")
    if s.check(st is not None, "InputScript drives the car (speed rises, "
                               "no control-socket input sent)"):
        s.framedump("inputscript")
    s.end_race_best_effort()

s.finish()
