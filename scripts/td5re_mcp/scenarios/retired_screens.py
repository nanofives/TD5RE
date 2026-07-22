"""retired_screens -- jumping to a retired frontend screen slot redirects to
the main menu instead of showing a dead/garbage screen (pending row 110).

Retired StartScreen/set_screen slots (e.g. 3, 4) must redirect to MAIN_MENU
(td5_frontend set_screen redirect, "set_screen(N): retired screen —
redirecting to ..."). Asserts set_screen on those slots lands on the main
menu (5), and a known-live screen still resolves to itself (control).
"""
from _lib import Scenario, STATE_MENU

SCREEN_MAIN_MENU = 5
RETIRED = (3, 4)

s = Scenario("retired_screens")

if s.check(s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30,
                        "frontend (MENU)") is not None, "frontend reached"):
    for n in RETIRED:
        res = s.cmd("set_screen", {"screen": n})
        # set_screen replies with the resulting screen index after redirect.
        landed = res.get("screen", -1) if res.get("ok") else -1
        s.check(landed == SCREEN_MAIN_MENU,
                f"retired screen {n} redirects to main menu (got {landed})")
    # Control: a live screen resolves to itself (proves redirect is targeted).
    res = s.cmd("set_screen", {"screen": SCREEN_MAIN_MENU})
    s.check(res.get("ok") and res.get("screen") == SCREEN_MAIN_MENU,
            "live screen 5 resolves to itself (no spurious redirect)")

s.finish()
