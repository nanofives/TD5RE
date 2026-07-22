"""gpu_device_lost_recovery -- a forced GPU TDR mid-race recovers instead of
black-screening / crashing (pending_to_test.csv lines 20 + 14).

TD5RE_FORCE_DEVICE_LOST=<frame> simulates a device-hung TDR on that present
frame. The wrapper must recreate the device and keep rendering: the sim must
tick PAST the forced frame, the run must reach MENU, no NEW crash.log may be
written, and recovery is logged ("DEVICE RECOVERED" -> log/gpu_device_lost.log,
written by d3d11_backend_device.c). Env is set before launch, so this scenario
always launches its own instance.
"""
import os
import sys

FORCE_FRAME = 900
os.environ["TD5RE_FORCE_DEVICE_LOST"] = str(FORCE_FRAME)   # inherited by the launch

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_MENU, REPO_ROOT

TRACK_MONTEGO = 17                          # the reported TDR track

s = Scenario("gpu_device_lost_recovery")
if not s._owns_proc:
    print("[gpu_device_lost_recovery] FAIL: needs a fresh launch "
          "(TD5RE_FORCE_DEVICE_LOST must be set at process start) -- close the "
          "running game first")
    sys.exit(1)

crash_log = REPO_ROOT / "crash.log"
crash_pre = crash_log.stat().st_mtime_ns if crash_log.exists() else None

if s.check(s.start_race_and_wait(track=TRACK_MONTEGO, game_type=GAMETYPE_SINGLE_RACE,
                                 laps=1, opponents=3, traffic=1,
                                 player_is_ai=1, auto_throttle=1),
           "Montego AutoRace launched (device-lost armed at frame 900)"):
    # Survival: the sim must keep advancing well past the trigger frame.
    st = s.wait_until(
        lambda x: x.get("race", {}).get("sim_tick", 0) > FORCE_FRAME + 200,
        90, "sim ticks past the forced device-lost frame")
    if s.check(st is not None, "game keeps running through the TDR (no freeze)"):
        s.framedump("post_recovery")        # non-black backbuffer == still rendering

    # Recovery logged, and no fresh crash.log.
    recovered = False
    for name in ("gpu_device_lost.log", "engine.log", "race.log"):
        p = REPO_ROOT / "log" / name
        if p.exists() and "DEVICE RECOVERED" in p.read_text(errors="ignore"):
            recovered = True
            break
    s.check(recovered, "log records 'DEVICE RECOVERED'")

    crash_now = crash_log.stat().st_mtime_ns if crash_log.exists() else None
    s.check(crash_now == crash_pre, "no new crash.log written during the TDR")

    s.cmd("end_race")
    s.wait_until(lambda x: x.get("game_state") == STATE_MENU, 45, "back at MENU")

s.finish()
