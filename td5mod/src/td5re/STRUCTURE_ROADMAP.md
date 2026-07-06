# TD5RE structure roadmap (2026-07-06 batch)

Target end-state for the architecture cleanups started in the 2026-07-06
structure-guards batch, so any future session can continue instead of
re-deriving. The **ratchets in `scripts/lint_structure.ps1` enforce that none
of this regresses** (CI fails on: new extern-in-.c, new `td5_game.h`
includers, new warnings per -W class). Lower a metric → lock it in with
`-UpdateBaseline`.

## 1. Layering (dependency direction)

```
platform (td5_platform_win32)        — includes NO game headers (already clean)
   ↑
services: asset, track, inflate, font, jobs, rcmd, config, save
   ↑
sim: physics*, ai, camera, light, sound, vfx
   ↑
game core: td5_game (race FSM), net, arcade, damage, trace
   ↑
presentation: frontend*, fe_*, hud, selftest
```

- Leaf modules query race state via **`td5_race_state.h`** (narrow, read-only
  subset of td5_game.h). 7 modules converted 2026-07-06 (32 → 25 includers).
- Remaining `td5_game.h` includers that need **mutation redesign** first:
  - `td5_physics.c` — writes `g_actor_pool` (pool-base ownership belongs in
    td5_game.c; move the assignment, then convert).
  - `td5_sound.c` — calls `td5_game_advance_sky_rotation()` from the siren
    path (relocate the mutator or invert to an event flag).
  - `td5_ai.c` — cop-chase mutators (`add_wanted_score/kill`,
    `set_arrest_time`, `infect_request`) + writes `g_td5.total_actor_count`.
    Needs a small write-API header (`td5_race_events.h`?) distinct from the
    read-only one; never widen td5_race_state.h with mutators.
- Everything ABOVE game core (frontend/hud/selftest) legitimately includes
  td5_game.h; don't churn those.

## 2. g_td5 god-struct (~57 fields + ~142-knob `ini` sub-struct)

Direction (per the 2026-07-02 audit + 2026-07-06 measurements: 2,165 access
sites; game.c 571, main.c 451):

- **Config (`g_td5.ini`)**: knobs stay physically in the struct (main.c's
  INI/CLI table depends on the flat layout) but each block is OWNED by one
  module — see the ownership contract comment on the struct. End-state: a
  module reads its own knobs via a `const TD5_XxxConfig *td5_config_xxx(void)`
  view; cross-module knob reads are a smell.
- **Runtime state**: migrate per-module clusters OUT of g_td5 into
  module-owned structs with accessors. Precedent: `g_rs` (render split P1-C,
  thread-local + `s_*` shims) — copy that pattern. Good next candidates:
  benchmark/trace fields (dev-only, low risk), then camera, then sound.
- Never reorder existing fields casually: some RE-ported code paths rely on
  grouping for readability against Ghidra offsets (layout itself is NOT
  ABI-critical — g_td5 is port-only — but diff noise buries real changes).

## 3. td5_game.c god-functions

`td5_game_init_race_session` (~2.7k LOC, already organized as logged
"InitRace step N/19" phases) and `td5_game_run_race_frame` (~1.8k LOC,
trace-stage seams) → static phase functions, PURE code motion, verified by:
build + selftest + **golden traces** (`trace_goldens.txt` hashes catch any
sim divergence — see CLAUDE.md "Golden traces").

## 4. Verification ladder for structural work

1. `build_all.bat` (both variants; -Werror set catches the dangerous classes)
2. `pwsh scripts/lint_structure.ps1` (ratchets; report-only locally, CI fails)
3. `pwsh scripts/selftest.ps1` (smoke) / `-Suite full` (+ golden traces)
4. For sim-touching refactors: goldens MUST match; if an intentional sim
   change, re-record via `TD5RE_TRACE_GOLDEN_UPDATE=1` and commit the file
   with the change.
