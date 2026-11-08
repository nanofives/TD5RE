# Round S (roam-SA): the strip geo BUILD is deleted (2026-10-30)

Mariano, asked "start the round that deletes the old track builder?": "si". Branch roam-SA, base master
d32da5ed. Map: `R_SPLIT_DELETABLE.md` (section 1 of `F11_REMOVAL_MAP.md`, re-measured 25 Oct).

Since round R no default path built level091: local split screen races the free-roam city and the strip
was only reachable with `TD5RE_GEO_STRIP_BUILD=1`. That knob, the BUILD, the strip race on a geo slot and
every module only they used are gone. The geo map (place pick, pins, RACE ROUTE, FREE ROAM setup, SPRINT /
CIRCUIT settings), roam races (1 player and split screen), the auto track (DefaultTrack 60) and every
shipped track stay.

## Line counts (`wc -l`, td5mod/src/td5re/*.c *.h, before d32da5ed / after)

| | before | after | delta |
|---|---|---|---|
| **all .c / .h** | 417 000 | 400 226 | **-16 774** |
| verify / re/tools / inputscripts deleted (91 files) | 13 061 | 0 | -13 061 |

Whole modules deleted (17 files, 11 101 lines): td5_tg_realfork.c 1899, td5_tg_plazafork.c 1727,
td5_tg_avenue.c 1584, td5_tg_blockloop.c 1427, td5_tg_geo_street.c 1280, td5_tg_geo_clear.c + .h 831 + 127,
td5_geo_attrs.c + .h 523 + 149, td5_geo_avenues.c + .h 466 + 91, td5_geo_forks.c + .h 274 + 80,
td5_roam_view.c + .h 462 + 36 (D5), td5_geo_fork_verdicts.h 75, td5_dev_forkfilter.h 70.

Trimmed (before / after): td5_geo_route.c 6481 / 4704, td5_tg_city.c 7382 / 6898, td5_fe_geo.c 2562 / 2122,
td5_roam_height.c 653 / 281, td5_track.c 12677 / 12316, td5_tg_branch.c 1928 / 1605, td5_trackgen.c
6014 / 5762, td5_tg_road.c 2383 / 2133, td5_tg_network.c 3852 / 3649, td5_fe_race.c 12772 / 12579,
td5_trackgen_internal.h 6473 / 6308, td5_geo.c 1307 / 1171, td5_hud.c 9918 / 9799, td5_tg_streets.c
7594 / 7492, and 34 more files by under 100 lines each (full table: `_archive/roam/SA/wc_table.txt`).

The removal map's ~16.8k estimate is met by a different split: td5_geo_route.c lost 1 777 lines, not ~5 500,
because the ROUTER and CONDITIONER (`td5_geo_route_build`, ~3 000 lines) stay: the geo map still draws the
routed line and its length from it. The extra cuts came from the shared modules' strip islands (below).

## What went

| What | Notes |
|---|---|
| Section 1 modules | attrs, avenues, forks, geo street, geo clear, real / plaza / block-loop forks, avenue (table above) |
| td5_geo_route.c: the COMMIT | `td5_geo_route_commit` + `_commit_reason`, the JSON writers (ROUTE, ROUTE_RAW, FORKS, AVENUES), the raster re-grid / re-project, the derived PLACE, the dev parity harness `gr_self_test` (`TD5RE_GEO_ROUTE_TEST`), `td5_geo_route_is_stub` |
| D6 `td5_geo_derived_migrate` | CUT (it re-committed old `_route/` frames; nothing writes one now). td5_geo_sync no longer calls it |
| D5 dev roam code on the derived frame | CUT: td5_roam_view.c (roam tiles drawn over a strip race), the roam renderer's route-frame mode (`TD5_RoamRenderFrame.route`, `TD5RE_ROAM_RENDER=1` overlay, `TD5RE_ROAM_RENDER_HIDE_STRIP`), the route arm of the pose generator (`TD5RE_ROAM_RENDER_POSES` now needs `_SPAWN`), the height self-test `td5_roam_height_selftest` (`TD5RE_ROAM_HEIGHT_TEST`) |
| td5_fe_geo.c | BUILD TRACK (`geo_commit_and_register`, `geo_do_build`), `TD5RE_GEO_AUTOBUILD(_N)`, the strip mode `s_strip`, the generation pages of the OPTIONS panel (AVENUES, RUN-OFF, FORKS, LOOPS, SIGNALS, CROSSWALKS, BUS STOPS, FOOTPATHS, CITY, BUILDINGS, PLAZAS, PARK TREES, STREET LAMPS), `TD5RE_GEO_PANEL_SECT` / `_PANEL_SET`, the strip verdict status branch, the stub label. The section selector and DEFAULTS buttons stay created and hidden so the button numbers (`TD5RE_GEO_PANEL_FOCUS`) do not move |
| td5_fe_race.c / td5_frontend.c | `frontend_geo_strip_context()` and its 9 call sites, `frontend_geo_entry_prepare_strip` (strip on demand), the geo pick arm (`frontend_geo_pick_arm` / `_slot`, only BUILD armed it), the strip route preview in `td5_geo_draw_route`, the AutoRace strip branch |
| The knob | `TD5RE_GEO_STRIP_BUILD`, `td5_roam_strip_build_enabled`. Also `TD5RE_STUDIO_SYNTHETIC` (the auto slot always builds the synthetic world) and `TD5RE_GEO_ROUTE` (a route over the synthetic world) |
| Race on a geo slot outside roam | td5_game.c generates only the auto slot; a geo slot outside a roam session logs an error and races track 0 (no menu path reaches it). `td5_trackgen_set_target_slot` targets the auto track only |
| td5_track.c | the XSPAN crossing-safe localiser (`xspan_*`, `GeoQuad.cy`, the census, the walker far-jump revert) |
| td5_geo.c | the SELECT TRACK route preview (`td5_geo_preview_route` / `_node`) |
| AI / HUD | the OSM `maxspeed` caps (td5_ai.c, td5_ai_driver.c, td5_ai_traffic.c), the HUD "ON <street>" line, the minimap label own-street exclusion |
| Shared generator islands that called section 1 | tg_branch: geo / real / plaza / loop forks (`TG_Fork.real/freec/loopc` removed), `tg_fork_real_row_shift`, `tg_fork_left_real_median`, the inner block-loop pavement, `tg_br_sidewalk_on_street`; tg_road: the route walk `tg_geo_walk` and its plaza lane floor; trackgen: the MGAP / SLNK trailers, the plaza corridor emit arms, the geo street emitters and reports; tg_city: the strip building emitter (`tg_geo_emit_one` / `_buildings`, the strip callbacks), the avenue far kerb, the avenue railing rule, the clear-gap and box/wall clearance calls; tg_network: the plaza far-arc streets; tg_streets / tg_terrain / tg_furniture / tg_prefab / tg_guard / tg_bridge / tg_pages: the avenue and fork queries |

Each call site was replaced by what the deleted function returned with no place loaded (a synthetic build),
so the auto track's bytes cannot move; the gate below confirms it.

## Scripts and tools deleted (91 files, list in `_archive/roam/SA/scripts_delete.txt`)

- verify/geo_*: 59 of 67 (all the level091 harnesses: attrs, bl_*, bld_*, commit_*, frames, fork_*,
  gapdrive, gen_measure, lf_trace, n2_*, park_cover, peer_direction, r1014a..r1022q, r3_frames, realfork_*,
  region_screen, strip_plot, world_frames). Kept: geo_tags_identity.ps1 (`-Arm synthetic`),
  geo_level_identity.py, geo_tour.ps1, geo_r1014a_poses.py (geo_tour's -Spans reader), geo_reuse_repro.ps1
  (level090), geo_offline_regen.py (place data), geo_lf_poses.py, geo_r1016k_geom.py (STRIP.DAT readers).
- Their dependents: verify/r1015c_{align,joint_step,pave_gap,plaza_cover,tilt,tour}, r1021c_* (6),
  r1023c_bld_intrude, r1023e_* (3), r1024i_* (2), r1029x_route_search, r1030e_run; verify/xspan_* (5).
- re/tools strip route chain: geo_condition.py, geo_route.py, geo_forks.py, geo_selector.py +
  geo_selector/index.html (the browser selector's SEND TO GAME wrote ROUTE.JSON for the strip).
- inputscripts/f11b_mp_strip.txt.

## Stayed, and why

| What | Why |
|---|---|
| Section 2 islands still gated on `td5_geo_loaded()` / `td5_geo_route_count()` in tg_network (`tg_net_geo_*`), tg_streets (geo lawns, parks, plazas), tg_city (`tg_geo_city_prepare`, `tg_geo_mass_at`, sidewalk widths), tg_furniture (signals), tg_terrain, tg_bridge, tg_guard, trackgen (`tg_geo_apply_spec`, `tg_geo_track_name`, `tg_spec_hash` route fold, `tg_geo_sky_profile`), minimap_streets (geo branch), td5_geo_buildings (route binding) | DEAD now (the generator never runs with a place: set_target_slot pins the synthetic world) but each is a module of its own to trim with the synthetic gate after each. Brief rule: trim only what is provably strip-only, one module at a time. Next round |
| `td5_geo_route_build` (router + conditioner) | the geo map draws its routed line and the "ABOUT x KM" length |
| Section 3 (place outlines, raster window reader `gr_raster_read_window`, `gr_resample`) | roam reads them |
| The geo slots and their registration (ROAMGRID) | the GEOSPATIAL / FREE ROAM entries and the roam host need a registered slot |
| td5_track.c MGAP / SLNK / CMRP readers | the engine side of the deleted trailers: inert with no trailer, next round |
| verify/roam/f4a_tour.ps1, f4c_tour.ps1 `-Strip`, f5a_ab.ps1 `-Geo`, verify/geo_tags_identity.ps1 `-Arm geo`, geo_tour.ps1's default `-Track 61` (D8) | `.ps1` files cannot be edited on this account (claude2). Those arms no longer work: run the roam arms, `-Arm synthetic`, `geo_tour -Track 60`. verify/roam/f9b_trace_ab.py (Python) lost its strip arm |

## Evidence (all in `_archive/roam/SA/`)

- Synthetic level090 (SELECTED.TXT aside, seed 99991, `verify/geo_tags_identity.ps1 -Arm synthetic
  -NoControl`): master exe and the new exe, `python verify/geo_level_identity.py level090_master
  level090_sa1`: **bytes IDENTICAL**, MODELS sha256 E2F1F33C221D61CAB0EFF701484011F0, md5 43FA31619D6DC5A4
  (`id_syn_master.txt`, `id_syn_sa1.txt`). Rerun on the final exe: `id_syn_final.txt`.
- `build_all.bat` (absolute path): DEV + RELEASE OK, structure lint extern 3/3, td5_game.h includers 23/23,
  warnings 84/84 (`build_all_final.log`). Smoke on that exe: 22 PASS / 0 WARN / 0 FAIL
  (`selftest_smoke_final.txt`); synthetic level090 on that exe: IDENTICAL to master (`id_syn_final.txt`).
- Selftest smoke: 22 PASS / 0 WARN / 0 FAIL incl. roam, roam-sprint(-ai)-smoke, roam-circuit-smoke,
  roam-sprint-mp2, roam-circuit-mp2, roam-traffic-smoke, roam-cops-smoke (`selftest_smoke.txt`).
- Selftest full: 87 PASS / 0 WARN / 2 FAIL (`selftest_full.txt`). `roam-cops-roadblock` FAILs on the MASTER
  exe too, same verdict ("scenario 3", run alone on both exes). `suite-global-timeout`: the suite's own 900 s
  deadline cut off roam-partido, ovr-edinburgh-finish and rt-sydney; run alone with
  `TD5RE_SELFTEST_TIMEOUT_S=2400` all three PASS (`selftest_tail_*.txt`).
- Geo map through the menus (`verify/roam/f2b_run.py`, `inputscripts/f11a_geo_race_route.txt`, two seeded
  pins): `--StartScreen=56` RACE ROUTE -> car select -> `roam: session start ... host_track=61`, a city race
  with route walls (`out/geo_race_route`); `--StartScreen=57` FREE ROAM setup, CONFIRM -> car select -> free
  roam (`out/free_roam`).
- Master roam probe (`roam-sprint-ai-smoke` on the master exe): `geo: route .../_route/ROUTE.JSON loaded:
  1330 nodes`, `geo_corners: ARMED`, no `[GEO ATTRS]` race tables, no `[XSPAN]` census: deleting attrs and
  xspan does not change a roam race.

## Follow-up (orchestrator, same round): the AI no longer depends on the leftover `_route/ROUTE.JSON`

What each of the three racer-AI behaviours read from the stale strip route, and what roam did with it:

| Behaviour (td5_ai.c) | Read from the route | In a roam race | On a shipped track with a place selected |
|---|---|---|---|
| geo corner edge (`s_geo_gov`, `TD5RE_AI_GEO_EDGE`): the lane clamp kept 900 units off each rail | only "route loaded" (`td5_geo_route_count() >= 2`), plus a generated slot; the margin came from the TRACK's own span frame | armed at race init (master logs `geo_corners: ARMED`) but NEVER APPLIED: a roam race skips `td5_ai_tick()` (td5_game.c, roam runs `td5_roam_ai_tick`), so the lane brain that reads `s_geo_gov` does not run | off (needs a generated slot) |
| time-based fork look-ahead (`TD5RE_AI_BRANCH_LOOK_SEC`, +1.5 s of travel added to the 7..15 span window) | only "route loaded" | never runs (same reason) | **ON by accident**: SELECTED.TXT loads the place and its route at boot, so every shipped-track race widened the fork window |
| geo finish brake (`TD5RE_GEO_FINISH_BRAKE`): an AI car past the line brakes to a stop | only "place + route loaded" | never runs (roam has its own finish, td5_roam_race.c) | **ON by accident** on point-to-point shipped tracks |

None of the three read any route DATA (nodes, names, speeds): only its presence. All three existed for the strip
geo track, which is gone, so all three are DROPPED (not re-gated). Roam is unchanged by construction, and a
shipped track no longer changes behaviour with the place selection.

With that, nothing needed the route loader: `td5_geo.c` no longer reads `_route/ROUTE.JSON` (or its grade
separations, `td5_geo_xsep`), and its readers went too: the generator's grade-separation planner
(`tg_xsep_*`, td5_tg_road.c / td5_tg_bridge.c), the route-derived finish span, track name and spec-hash fold
(td5_trackgen.c, `tg_geo_apply_spec` is now `tg_geo_sync_place`), the land-cover biome override (td5_tg_terrain.c),
the painted finish line (td5_tg_furniture.c), the mast-arm plate planner (td5_tg_mastarm.c; roam keeps the
`tg_plate_*` text helpers), the footprint-to-route binding (td5_geo_buildings.c; free roam reads footprints per
tile), the minimap's geo street source (td5_minimap_streets.c; free roam has its own street map) and the plaza
span report (td5_tg_streets.c). The route-gated generator islands that only test `td5_geo_loaded()` stay
(dead: the generator never runs with a place).

Also removed this pass: the AUTO TRACK STUDIO's hidden LOCATION row (`AT_KIND_PLACE`) and `td5_geo_select`, the
last writer of SELECTED.TXT (td5_geo_init still reads a file left on disk). Pending CSV: 64 more strip-only rows
from before round 1014 superseded (145 in total, `_archive/roam/SA/csv_supersede2.txt`).

### Follow-up evidence (`_archive/roam/SA/`)

- Lines: all td5re `.c` / `.h` now 398 079 (417 000 at d32da5ed: **-18 921**).
- Roam AI, master exe vs this branch, every roam row with `TD5RE_FIXED_FRAME_US=16667`
  (`roamab_*.md`, `roamab_compare.txt`), then roam-sprint-ai / roam-partido / roam-traffic /
  roam-circuit-smoke twice more on each exe (`roamab_repeat.txt`). Roam runs are NOT repeatable even with
  the frame delta pinned (the stream worker is wall-clock): master against itself moves the
  roam-sprint-ai gaps (C2..C5 +28.83/+49.23, +28.50/+40.60, +36.63/+38.00 over three runs) and FAILs
  roam-traffic once (rep 2). Every difference between the exes sits inside that spread:
  roam-sprint-ai-smoke, roam-sprint-mp2 and roam-sprint-mp4 are identical on every run; roam-traffic on
  this branch reps 2 and 3 equals master rep 3 to the digit; roam-partido passed on reps 2 and 3 (its one
  "stream misses 2" FAIL is the same streaming timing); the lap times of roam-circuit-smoke differ by
  0.13..0.26 s between runs on both exes. Corner-limit hits: none on either exe, because the governor's code
  never runs in roam (`td5_ai_tick` is not called); master's `geo_corners: ARMED` line was the only trace.
- Shipped track with a place selected (`place_ab.py 0 2500 1`: Moscow, 5 AI, AI slot 1 traced, with and
  without SELECTED.TXT, `place_ab_t0_s1.txt`): MASTER diverges at sim tick 1191 (AI steering 5376 -> -1408,
  controls / driver / motion / pose / rotation / track); this branch: every sim-tick module identical
  (only the render-frame files differ, as on any two runs).
- Roam race with `_route/` moved aside (`noroute_result.txt`): roam-sprint-ai-smoke and roam-sprint-ai
  PASS, the first identical to every other run, the second equal to master rep 2; no route is read.
- Gates on the final exe: synthetic level090 IDENTICAL (`id_syn_followup.txt`), smoke 22 / 0 / 0
  (`selftest_smoke_followup.txt`), DEV 84 warnings, RELEASE builds.
- Frames (RT off, minimum graphics, RenderScale 100, map tiles offline so the map shows placeholders):
  `frames/geo56_panel.png`, `frames/geo56_sprint_circuit_pins.png`, `frames/roam57_free_roam_setup.png`,
  `frames/roam_race_route_tick30.png`, `frames/studio52.png`, `frames/studio52_terrain.png`.

Also changed by the loader cut, visible: the AUTO-GENERATED entry is always named AUTO-GENERATED (master
named it after the selected place at boot, because the strip route was loaded then).
