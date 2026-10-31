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
| td5_geo.c route loader (`td5_geo_route_count/node`, `_route/ROUTE.JSON`) | the racer AI's geo gates read it: a MASTER roam race logs `geo_corners: ARMED` because the strip's old `_route/ROUTE.JSON` (1330 nodes) is on disk and loads at boot. Deleting the loader would turn that governor off in roam. Kept so roam AI is unchanged; see open questions |
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
- Compile: DEV 84 warnings (baseline 84), RELEASE builds (`pbuild3.log`, `pbuild3r.log`).
- Selftest smoke: 22 PASS / 0 WARN / 0 FAIL incl. roam, roam-sprint(-ai)-smoke, roam-circuit-smoke,
  roam-sprint-mp2, roam-circuit-mp2, roam-traffic-smoke, roam-cops-smoke (`selftest_smoke.txt`). Full: see
  the round report.
- Master roam probe (`roam-sprint-ai-smoke` on the master exe): `geo: route .../_route/ROUTE.JSON loaded:
  1330 nodes`, `geo_corners: ARMED`, no `[GEO ATTRS]` race tables, no `[XSPAN]` census: deleting attrs and
  xspan does not change a roam race.
