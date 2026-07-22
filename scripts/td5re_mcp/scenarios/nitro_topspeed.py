"""nitro_topspeed -- ARCADE NITRO raises the top-speed cap (~1.5x) while active
(pending row 88 / "Arcade NITRO: glowing speed trail + 1.5x top speed").

The trail is visual (not observable), but the raised cap is: while a racer holds
NITRO (arcade_effect == TD5_PU_NITRO = 1) it can exceed the normal top speed
(td5_arcade_slot_topspeed_pct = 150%). Run an arcade race with a full field,
lengthen the NITRO window (TD5RE_ARCADE_NITRO_FRAMES) so it's easy to catch a
car at speed under it, and assert the fastest speed seen UNDER NITRO clearly
exceeds the fastest speed any car reaches with NO power-up.
"""
import os
import time

# Long NITRO windows so an accelerating car actually reaches the raised cap
# within one straight before it expires (default 180 ticks = ~6s).
os.environ["TD5RE_ARCADE_NITRO_FRAMES"] = "600"

from _lib import Scenario, GAMETYPE_SINGLE_RACE, STATE_RACE

NITRO = 1                    # TD5_PU_NITRO (td5_arcade.h)

s = Scenario("nitro_topspeed")
s.cmd("set_param", {"name": "trace_fast_forward", "value": 6})   # applies next race

if s.check(s.start_race_and_wait(track=0, game_type=GAMETYPE_SINGLE_RACE,
                                 laps=3, opponents=5, traffic=2, dynamics=0,
                                 player_is_ai=1, auto_throttle=1),
           "arcade race launched (Moscow, dynamics=0, 5 AI, 6x FF)"):
    baseline_top = 0         # fastest speed seen with NO effect active
    nitro_top = 0            # fastest speed seen while a racer holds NITRO
    saw_nitro = False
    samples = 0
    end = time.time() + 90
    while time.time() < end:
        st = s.state()
        if st.get("game_state") != STATE_RACE:
            break
        for r in st.get("race", {}).get("racers", []):
            spd = r.get("speed", 0)
            eff = r.get("arcade_effect", 0)
            if eff == NITRO:
                saw_nitro = True
                nitro_top = max(nitro_top, spd)
            elif eff == 0:
                baseline_top = max(baseline_top, spd)
        samples += 1
        time.sleep(0.3)

    print(f"[{s.name}]   samples={samples} saw_nitro={saw_nitro} "
          f"baseline_top={baseline_top} nitro_top={nitro_top}")

    if s.check(saw_nitro, "a racer acquired NITRO during the arcade race"):
        s.check(baseline_top > 0, "measured a no-power-up baseline top speed")
        # NITRO raises the cap to 150%; require a clear margin over the fleet's
        # normal top speed (conservative: cars don't always reach the full cap).
        s.check(nitro_top > baseline_top * 1.15,
                f"NITRO lets a car exceed the normal top speed "
                f"({nitro_top} > 1.15 x {baseline_top})")

    s.end_race_best_effort()

s.finish()
