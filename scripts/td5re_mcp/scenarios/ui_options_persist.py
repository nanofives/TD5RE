"""ui_options_persist -- interactive menu nav + whitelist knob round-trip.

Selftest's screen walk checks reachability via internal nav; this drives the
frontend with REAL injected keys and asserts the screen index responds, plus
a live set_param/get_param round-trip on the `laps` knob.

FINDINGS from this scenario's first runs (kept as documentation):
- td5_ini_persist_options() runs only from the options screens' OK/back
  paths, NOT at shutdown — so "set_param + quit" does NOT persist to
  td5re.ini. A true UI persistence test must cycle a row on the options
  screen and press OK; that's deferred until the RACE OPTIONS consolidation
  lands (rows are about to move) — tracked as auto-blocked in the triage.
- Audio volumes are additionally owned by the SAVE layer (re-applied at race
  transitions), so they are the wrong persistence probe entirely.
- After a screen transition begins, injected keys during the fade can be
  flushed — settle briefly and retry taps instead of single-shot.
"""
import time

from _lib import Scenario, STATE_MENU

SCREEN_MAIN_MENU = 5
PROBE_VALUE = 7           # unusual laps count; restored afterwards

s = Scenario("ui_options_persist")

# --- 1. interactive nav: tap real keys, watch the screen index move --------
if s.check(s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 30,
                        "MENU state") is not None, "frontend reached"):
    s.cmd("set_screen", {"screen": SCREEN_MAIN_MENU})
    time.sleep(0.8)
    scr0 = s.state().get("screen")
    s.cmd("tap_key", {"dik": 0x1C})                     # ENTER: activate row
    st = s.wait_until(lambda x: x.get("screen") != scr0, 10,
                      "screen changes after ENTER")
    s.check(st is not None, f"ENTER navigates away from main menu (screen {scr0})")
    if st is not None:
        time.sleep(0.8)                                 # let the transition settle
        # keys tapped during a fade can be flushed -> retry the ESC
        returned = None
        for _ in range(4):
            s.cmd("tap_key", {"dik": 0x01})             # ESC: back out
            returned = s.wait_until(lambda x: x.get("screen") == scr0, 3,
                                    "screen returns after ESC")
            if returned:
                break
        s.check(returned is not None, "ESC returns to the previous screen")

# --- 2. whitelist knob round-trip (live) -----------------------------------
orig = s.cmd("get_param", {"name": "laps"}).get("value")
if s.check(orig is not None, f"read current laps ({orig})"):
    s.cmd("set_param", {"name": "laps", "value": PROBE_VALUE})
    got = s.cmd("get_param", {"name": "laps"}).get("value")
    s.check(got == PROBE_VALUE, f"laps set_param round-trip ({got} == {PROBE_VALUE})")
    s.cmd("set_param", {"name": "laps", "value": orig})
    s.check(s.cmd("get_param", {"name": "laps"}).get("value") == orig,
            f"laps restored to {orig}")

s.finish()
