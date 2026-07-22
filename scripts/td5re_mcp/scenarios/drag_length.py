"""drag_length -- TD5RE_DRAG_LENGTH_LEVEL makes the drag race proportionally
longer, reusing the existing road (pending row 166).

The length LEVEL (0=SHORT 1=MED 2=LONG 3=EPIC) is read at InitRace from
getenv, so it is fixed per process -> two full launches. Rather than drive to
a natural finish (fragile, and doubly exposed to the flaky first-launch GPU
TDR), we read the strip config straight from get_state: race.drag_repeats =
extra spans inserted into the strip (0 = SHORT / unchanged, >0 = a longer
strip). EPIC must insert more road than SHORT.
"""
import os

from _lib import Scenario, GAMETYPE_DRAG_RACE


def repeats_for(level: int):
    """Launch with a given length level, read race.drag_repeats at race start,
    tear down. Returns (repeats or None, Scenario)."""
    os.environ["TD5RE_DRAG_LENGTH_LEVEL"] = str(level)
    s = Scenario(f"drag_length_L{level}")
    if not s._owns_proc:
        print(f"[drag_length] FAIL: level {level} needs a fresh launch "
              "(env fixed at spawn) -- close the running game first")
        s.client.close()
        return None, s
    rep = None
    if s.start_race_and_wait(track=0, game_type=GAMETYPE_DRAG_RACE,
                             opponents=1, player_is_ai=1, auto_throttle=1):
        rep = s.state().get("race", {}).get("drag_repeats")
    import launcher
    launcher.stop(s.proc, client=s.client)      # tear down WITHOUT a verdict
    s.client.close()
    return rep, s


short_rep, s = repeats_for(0)      # SHORT (unchanged strip)
epic_rep, _ = repeats_for(3)       # EPIC (maximally extended)

s.checks = []
print(f"[drag_length]   SHORT(L0): drag_repeats={short_rep}")
print(f"[drag_length]   EPIC (L3): drag_repeats={epic_rep}")

if s.check(short_rep is not None and epic_rep is not None,
           "both drag runs reported strip config (drag_repeats)"):
    s.check(short_rep == 0, f"SHORT inserts no extra road (repeats {short_rep} == 0)")
    s.check(epic_rep > short_rep,
            f"EPIC extends the strip beyond SHORT (repeats {epic_rep} > {short_rep})")

s.proc = None
s._owns_proc = False
s.finish()
