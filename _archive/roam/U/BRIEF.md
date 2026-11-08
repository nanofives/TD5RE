# Round U (2026-11-08): general game feedback - round brief

Read `_archive/roam/BRIEF_COMMON.md` first (account rules, worktree, build, gates, report). Differences:
- **Base: `master` @ 8b5e802b** (rounds S + T merged). Branches `round-UA`, `round-UB`, worktrees
  `.claude/worktrees/round-UA` / `round-UB` (`git worktree add -b round-UA .claude/worktrees/round-UA 8b5e802b`, then
  `pwsh -NoProfile -File scripts/worktree_setup.ps1` inside it).
- First line of your first reply: your model id.
- CHANGELOG + pending_to_test.csv rows tagged "round U<A|B>", one row per item, written so Mariano can check by playing.
- Every claim of "fixed" needs a frame you looked at (framedump / screenshot) or a log/trace line. Read the PNG.
- Build DEV only while iterating (`fastbuild.bat <file.c>`, full `build_standalone.bat` after a header edit),
  `build_all.bat` once at the end. Long runs in background, no sleep-polling. WIP-commit per milestone.
- If something needs Mariano's decision, finish everything else, then put the question in your final report with
  the options and your recommendation. Do not stall waiting.

Mariano's feedback (2026-11-08, verbatim, numbering his):
1. i need you to recheck all the options for RT lightning, investigate if a refactor is necessary and be sure to make
   it work at stable FPS on older GPUs, recently I've tried in my 3070 and it struggles to load and have stable FPS
2. put lightning options under performance
3. always keep KPH as the default
4. remove randomize from the quick race menu on both car and track, going ONCE to the left on the default option
   (for car is the dodge viper, for track is moscow russia) it should go to the "Random" option, random of cars
   should be everything, random on tracks should be everything EXCEPT auto generated track OR la plata city free
   drive or the other mode
5. on time trials i selected traffic very high and it doesn't get reflecte on the race, make sure in every race mode
   every valid option is passed correctly, if not you can ask me and you can also ask me to remove incompatible options
6. (separate child, later) rework of the auto-track piece + texture set.

---------------------------------------------------------------------------------------------------------------------

## UA: RT lighting on older GPUs (items 1, 2)

1. **RT lighting on an RTX 3070: slow to load, unstable FPS.** This machine's GPU is faster; the target is a 3070
   (8 GB VRAM, Ampere, 1st-gen-ish RT throughput ~1/2-1/3 of what we develop on). Do a real audit, not a knob tweak:
   - Inventory EVERY RT/lighting option (LIGHTING OPTIONS sub-screen, td5_fe_menu.c ~1825: LIGHTING QUALITY master +
     per-feature tiers; PERFORMANCE / CUSTOM PERFORMANCE rows; INI keys; `TD5RE_RT*` / light2 knobs). For each: what
     it does on the GPU (passes, rays/pixel, resolution, denoiser cost), what it costs (use `td5_profile.c` per-phase
     timings + GPU timestamps if the backend has them; add GPU timestamp queries per RT pass if it does not), whether
     it actually works (dead options, options that do nothing, options that only apply after restart).
   - "Struggles to load": measure what RT adds to load time (BLAS/TLAS builds, shader/PSO compile, scenery feed,
     `td5_rt.c` coverage/streaming). PSO/shader compile at load is the usual suspect: check for a pipeline cache
     (ID3D12PipelineLibrary / cached blobs) and whether RT state objects are rebuilt per race.
   - "Unstable FPS": look for per-frame spikes, not just averages: BLAS rebuilds per frame, TLAS full rebuild vs
     update, descriptor/upload churn, scenery streaming stalls when RT is on, VRAM over-commit (8 GB budget: query
     `IDXGIAdapter3::QueryVideoMemoryInfo`, log usage; going over budget on a 3070 = paging = stutter).
   - Emulate a weaker GPU here as best you can: render at 1440p/4K, cap budgets, and report frame-time percentiles
     (p50 / p95 / p99 / max) per option with `td5_benchmark.c`, not just mean FPS.
   - Decide whether a refactor is needed and SAY WHY with numbers. Then deliver: (a) a sensible auto-detected default
     tier per GPU class (VRAM + RT tier + adapter name heuristics), (b) the fixes that remove spikes/load cost,
     (c) a "LOW/MEDIUM" RT tier that actually holds stable 60 fps on a 3070-class card at 1080p (half-res tracing,
     fewer bounces, temporal amortisation, etc.). If a big refactor is the honest answer, do the measurements + plan
     (`docs/plans/RT_OLDER_GPU_PLAN.md`) and the cheap wins, and report the plan for Mariano to approve.
   - Remember NON-SUITE launches default RT OFF + min graphics (memory rule); for this item you obviously test RT ON.
     Windowed, no focus steal, `--VSync=0` for timing (monitors may be asleep: vsync then reads 257 ms).
2. **Lighting options under PERFORMANCE.** Move the LIGHTING OPTIONS entry (now GRAPHICS OPTIONS row 6 ->
   TD5_SCREEN_LIGHTING_OPTIONS) so it lives under PERFORMANCE (screen 53) instead. Keep BACK returning to where you
   came from, keep button alignment (FE_MENU_BTN_* routine), no orphan row left in GRAPHICS OPTIONS. Update
   FRONTEND_SCREEN_GUIDE.md. Frame of both screens after.

## UB: quick race / race options (items 3, 4, 5)

3. **KPH always the default.** Find every place the speed unit default comes from (td5_save.c defaults, config load,
   INI, first-boot, cup/MP saves, the HUD) and make KPH the default everywhere, including when a save file has none
   or an old value from a previous default. Do NOT override a user's explicit MPH choice made in the options menu
   unless you find it was never really chosen (old default written to the save): say what you did and why.
4. **RANDOM as an entry, not a button.** Remove the RANDOMIZE buttons (td5_fe_race.c [#10]/[#14], ~288-1100, 1646,
   1858) from the quick race car and track selectors. Instead, pressing LEFT once from the default entry (car: Dodge
   Viper, track: Moscow, Russia) lands on a "RANDOM" entry (and RIGHT from it goes back). Car RANDOM = any car, all
   selectable cars. Track RANDOM = every track EXCEPT the auto-generated track, the La Plata city free drive, and the
   other generated / geo / roam modes (any geo slot, Gran La Plata sprint/circuit entries). Resolve the actual pick at
   race launch (each race re-rolls) and show what was picked in the loading screen / race. Preview for RANDOM: a "?"
   card or similar, consistent with the existing style. Check the same selectors in every place they appear (single
   player quick race, split-screen car select per player, time trial, cup if it shares the code). Remove dead code
   and the TD5RE_RANDOM_BUTTON / TD5RE_RANDOM_ICON knobs.
5. **Options not reaching the race.** Mariano: time trial with TRAFFIC = VERY HIGH, no traffic in the race. Audit
   EVERY race mode (single race, time trial, cup/championship, drag, cop chase, split screen, net/LAN, free-roam
   sprint/circuit) x EVERY race option (traffic, cops, laps, difficulty, AI count, direction/reverse, time of day,
   weather, damage, catch-up, units...) and produce a matrix: option offered in that mode? reaches the race? (log the
   value the race actually uses). Fix every option that is offered but lost. Where an option is offered but cannot
   work in that mode (e.g. traffic in time trial if the original forbids it), do NOT silently decide: list it as a
   question for Mariano with options "make it work" / "hide it in that mode" and your recommendation. Write the
   matrix to `docs/RACE_OPTIONS_MATRIX.md`.

---------------------------------------------------------------------------------------------------------------------

## Tests: one targeted test per item, NOT the whole suite (Mariano, 2026-11-08)
- Each item gets a test that checks exactly it: a selftest row in `td5_selftest.c` (named `u<item>-<what>`, e.g.
  `u5-traffic-timetrial`, `u4-random-track-pool`, `u3-kph-default`) asserting the behaviour, or a framedump/bench
  check for visual/perf items (1, 2). Say which kind each item got.
- Run ONLY your rows: `TD5RE_SELFTEST_ROWS="a,b" td5re.exe --SelfTest=1 ...` (or `TD5RE_SELFTEST_ONLY=<substring>`).
- Do NOT run smoke or the full suite; the orchestrator runs smoke once at integration. No synthetic identity unless
  you touched trackgen (you should not).

---------------------------------------------------------------------------------------------------------------------

## UC: sun placement vs the skybox (item 7, added 2026-11-08)

Mariano (verbatim): "sun placement on tracks should be careful to not be put somewhere where the background shows
something that's NOT sky, for that you would need to make a round of detection of what's in the skybox texture and
decide the correct position of the sun"

7. **Sun in the sky, never over mountains/buildings/ground of the backdrop.**
   - Find where the sun direction comes from per track (light2 / td5_light.c sun, RT sun, lens flare / sun disc
     billboard, any per-level table or knob) and how the skybox is mapped (texture, projection: which texel is at
     which azimuth/elevation from the camera). Say whether the sun disc, the flare and the lighting/shadow direction
     all share ONE direction; they must stay consistent after your change.
   - Build an offline classifier (Python under `re/tools/`) that reads each track's skybox texture(s) and labels
     texels sky / not-sky (colour + gradient + saturation heuristics; validate by eye on a contact sheet: write a PNG
     per track with the mask overlaid, look at all of them). Treat the sky edge with a margin so the sun disc + flare
     radius clears the skyline.
   - Choose per track a sun azimuth/elevation that lies fully in sky, as close as possible to the current direction
     (so lighting changes as little as possible), keep a believable elevation (not at the zenith, not touching the
     horizon). Output a table (e.g. `re/assets/.../SUNPOS` data file or a C table, your call, say why), loaded by the
     game, with the old direction as fallback when a track has no entry.
   - Scope: every shipped track (TD5 + TD6 if present, forward and reverse if the sky rotates), the auto track and
     the free-roam city (if their sky is procedural or a shared texture, apply the same rule: the sun must sit in
     sky where the backdrop has skyline/horizon geometry).
   - Proof: before/after framedumps looking at the sun for at least 4 tracks where it was wrong (find them with the
     classifier: report how many tracks had the sun over non-sky before), plus the contact sheet path. A selftest row
     `u7-sun-in-sky` that samples the skybox at the sun direction at race start and asserts sky.
   - Branch `round-UC`, worktree `.claude/worktrees/round-UC`, base 6610a463. Coordinate by not touching the RT
     option screens (UA owns them); if you must touch td5_rt.c / td5_light2.c keep the change small and say so.
