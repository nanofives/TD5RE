# Round V (2026-11-09): race flow, menus, shadows, free-roam sprint polish - round brief

Read `_archive/roam/BRIEF_COMMON.md` first (account rules, worktree, build, gates, report). Differences:
- **Base: `master` @ the commit that adds this brief** (round U + follow-ups shipped at c69e6d96). Branch `round-V<X>`,
  worktree `.claude/worktrees/round-V<X>` (`git worktree add -b round-VA .claude/worktrees/round-VA <base>`, then
  `pwsh -NoProfile -File scripts/worktree_setup.ps1` inside it, then `build_all.bat` ONCE by absolute path so the
  ddraw_wrapper lib exists).
- First line of your first reply: your model id.
- CHANGELOG (`td5_changelog.h`) + `pending_to_test.csv` rows tagged "round V<X>", one row per item, written so
  Mariano can check by playing.
- Every claim of "fixed" needs a frame you looked at (framedump / screenshot) or a log/trace line. Read the PNG.
- Build DEV only while iterating (`fastbuild.bat <file.c>`, full `build_standalone.bat` after a header edit),
  `build_all.bat` once at the end. Long runs in background, no sleep-polling. WIP-commit per milestone.
- Launches: windowed, no focus steal, `--VSync=0` (monitors may be asleep), RT off + min graphics unless the item
  is about rendering.
- If something needs Mariano's decision, finish everything else, then put the question in your final report with
  the options and your recommendation. Do not stall waiting.
- Siblings run in parallel: stay inside your files. Shared files (td5_changelog.h, pending_to_test.csv,
  td5_selftest.c rows) get union-merged by the orchestrator: add rows INSIDE the `k_races` array, never after it.

## Tests: one targeted test per item, NOT the whole suite
- Each item gets a test that checks exactly it: a selftest row in `td5_selftest.c` named `v<item>-<what>` asserting
  the behaviour, or a framedump sequence check for visual items. Say which kind each item got.
- Run ONLY your rows: `TD5RE_SELFTEST_ROWS="a,b" td5re.exe --SelfTest=1 ...`. A row without `.mp_mode = -1` is an
  MP race (drag got cleared that way once). Do NOT run smoke or the full suite.

Mariano's feedback (2026-11-09, verbatim, numbered in his order):
1. during menu animations when entering from one menu to the other (not coming back) for a frame the menu goes back and then disappears
2. there's no lap configuration on the latest version for circuits on time trials
3. high scores after exiting a drag race should be disabled (no high scores for drag strip)
4. sometimes there's perfect triangle shadows on the tracks (image: `_archive/roam/V/item4_triangle_shadows.png`)
5. when playing cop chase on single player after ending the race there shouldn't be a position on the end, just fade out
6. on the results it should say who escaped and who you rammed
7. delete random picture (image: `_archive/roam/V/item7_random_picture.png`)
8. on sprint on la plata map the arrows on the ground should be static and the yellow line should be narrower and more transparent, and the culling near the edges sometimes they stop rendering soon
9. there's issue with the wall arrows, sometimes they don't avoid going through a shortcut
10. the arrows with black background seem to represent nothing
11. remove red line on the end of a race
12. when finishing a race use the regular fade animation, the position (the star if it's first place), and use the regular menu for end of race with view race stats, replay, select new car, restart race and quit
13. avoid M ROUTE MAP

---------------------------------------------------------------------------------------------------------------------

## VA: frontend fixes (items 1, 2, 3, 7)

1. **One-frame flash of the old menu on a forward transition.** Entering a sub-menu (not going back), the outgoing
   menu reappears for one frame and then vanishes. Find it in the screen transition state machine (`td5_frontend.c`,
   FRONTEND_SCREEN_GUIDE.md first). Likely an ordering bug: the new screen's first frame draws before its slide-in
   offset is set, or the old screen is drawn once more at offset 0 after its slide-out ends. Proof: dump EVERY frame
   across several forward transitions (main menu -> race menu, race menu -> quick race, options -> graphics) and
   back transitions, before and after; list per frame which screen is drawn and at what offset. No frame may show the
   old menu at rest once its exit started. Check all forward paths, not one.
2. **LAPS for time trial on circuit tracks.** Round U (UB/UE) changed which rows time trial shows; the lap count row
   is gone. Bring it back for circuit tracks in time trial (hidden on point-to-point tracks), and prove the value
   reaches the race (row `v2-tt-laps`: time trial, circuit track, 2 laps, assert the race ends after lap 2).
   Update `docs/RACE_OPTIONS_MATRIX.md`.
3. **No high scores after a drag race.** After a drag strip race, skip the high-score entry / table entirely. Any
   other path that records drag times into the high-score tables goes too. Row `v3-drag-no-highscore`.
7. **Delete the RANDOM picture.** The RANDOM entry (car and track) shows a "?" card (see image). Remove the picture:
   keep the RANDOM title, show no preview image (no card, no frame). Check car select, track select, cup track picker
   and the split-screen car grid. Frames of each.

## VB: triangle-shaped shadows (item 4)

4. **Hard dark triangles on track geometry** (image: cop chase, inside a brick tunnel, dark wedges on the walls and
   ceiling). Most likely the RT-off screen-space sun-shadow march shipped in round UG (`ps_shadow`, G-buffer bound,
   knobs `TD5RE_SHADOW_GBUF`, `TD5RE_SHADOW_MARCH_YSIGN`, `TD5RE_SHADOW_DEBUG`, `TD5RE_SUN_SKY_SHADOWS`), but prove it:
   reproduce in that tunnel (find the track: brick tunnel with ceiling lamps; cop chase is just the mode), A/B with
   the UG pass off, and with RT on. Root-cause with evidence (why triangles: per-triangle normals, depth
   reconstruction, back faces, sun under the ceiling...). Then fix so tunnels and other enclosed geometry show no
   false shadows while the open-sky shadows from UG stay. Also sweep 6+ tracks for other triangle artifacts.
   Before/after frames of the same pose. Keep the cost about where UG left it (~0.1 ms at 1080p).
   Test: framedump check of the tunnel pose or a selftest row that measures it, your call, say why.

## VC: cop chase end of race (items 5, 6)

Single-player cop chase (the normal race mode, not free roam; check `td5_police.c` too if it is involved).
5. **No finishing position at the end.** When the cop chase ends, do not show a position (no "1ST", star or place):
   just the regular fade out.
6. **Results say who escaped and who you rammed.** The results / race stats screen for cop chase lists every
   speeder with its outcome: RAMMED / BUSTED (by you) or ESCAPED, plus whatever the mode already counts (points,
   time). Use the mode's real state, not a guess. Row `v6-copchase-results` asserting the per-car outcomes logged
   match the state, plus a frame of the results screen.

## VD: free-roam sprint route visuals (items 8, 9, 10, 13)

Free roam La Plata, SPRINT (and CIRCUIT if it shares the code). `td5_roam_route_ui.c`, `td5_roam_barrier.c`,
`td5_roam_race.c`, `td5_roam_render.c`. VE owns the race-end flow in `td5_roam_race.c`: coordinate by keeping your
edits there small.
8. **Route guidance look.** (a) The arrows painted on the ground must be static (no scrolling/animation). (b) The
   yellow route line: narrower and more transparent. (c) Culling: route arrows / line stop rendering too early near
   the screen edges; find the cull test (per-segment bounds too tight, a frustum test on a point instead of a box,
   distance cap...) and fix it. Frames before/after from the same pose, one of them looking along a curve.
9. **Wall arrows (route barriers) let you take a shortcut.** Sometimes a side street leaving the route is not closed,
   so the route can be cut. Find why the barrier placement misses those mouths (junction cluster, short link, a street
   that rejoins the route later, one-way...). Fix and prove on routes on the real map: report junctions passed, mouths
   closed and mouths open, before/after. Row `v9-barrier-no-shortcut`.
10. **Arrows with a black background mean nothing.** Identify them (texture/page, who places them, what they were
   meant for). If they carry no meaning, remove them. If they were meant to mean something, say what and fix the
   texture (no black box). Frame.
13. **No "M ROUTE MAP".** Remove the "M ROUTE MAP" prompt from the race HUD (`td5_roam_route_ui.c` ~1057; the results
   line in `td5_roam_race.c` ~1204 is VE's and goes away with the new end flow). The M key can keep working in FREE
   ROAM; just no on-screen prompt during a race.

## VE: free-roam race finish (items 11, 12)

11. **Remove the red line at the end of a race** (find it: finish line strip, route line turning red, a debug line).
   Frame before/after.
12. **Finish like a normal race.** When a free-roam race (sprint and circuit, 1 player and local MP) ends: the regular
   race fade animation, the finishing position the normal races show (the star for first place), then the regular
   end-of-race menu with VIEW RACE STATS, REPLAY, SELECT NEW CAR, RESTART RACE, QUIT, reusing the normal race code
   paths, not a copy. Each entry must work in roam: stats show the roam race's real times and positions; RESTART
   reruns the same route; SELECT NEW CAR goes to car select and back into the same route; QUIT goes back to where the
   race was started (generator / FREE ROAM). REPLAY: check whether `td5_replay.c` can record and play a roam race
   (streamed tiles, rebasing). If it works, wire it. If it needs real work, grey it out and put the cost in the report
   as a decision for Mariano. This replaces the current roam results panel (incl. "ENTER BACK TO THE GENERATOR
   M ROUTE MAP F5 RACE AGAIN"). Row `v12-roam-finish-menu` (sprint ends -> menu with the 5 entries, RESTART reruns),
   frames of the fade, the position and the menu.
