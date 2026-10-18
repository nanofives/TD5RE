# F11 removal map: what the strip geo BUILD leaves behind (F11a, 2026-10-18)

For F11b (the deletion). At roam-F11a the strip BUILD is only hidden behind
`TD5RE_GEO_STRIP_BUILD=1` (`td5_roam_strip_build_enabled`, td5_roam_handoff.c);
nothing is deleted. Line numbers are at f5940f1c + F11a and move: re-grep the
symbol before editing. Three consumers matter:

- **STRIP**: a real place driving the strip trackgen (level091+, slot 61+).
- **SYNTHETIC**: the auto track (level090, slot 60). **Stays.**
- **ROAM**: `td5_roam_*.c`. **Stays.**

`td5_track.c`'s `geo_*` (1816-2600) is **geometric** collision
(`TD5RE_GEO_COLLISION`), used on reverse native circuits and custom tracks incl.
level090: not part of this map, keep. Its `xspan_*` localiser (1730-1815) is
strip-geo-only (`td5_geo_route_count() > 0`).

## 0. Hidden dependencies: deleting the BUILD breaks roam unless F11b handles these

| # | Dependency | Where | What F11b must provide |
|---|---|---|---|
| D1 | Roam's host level IS the BUILD's output | `td5_trackgen_roam_host_slot` (td5_trackgen.c ~5106) needs level09x `TEXTURES.DAT`, `LEVELINF.DAT`, `FORWSKY.png`, else roam falls back to a shipped level's pages | a host-only writer (a subset of td5_tg_pages.c) or shipped host files per place |
| D2 | Roam's sky reads the BUILD's stamp | `td5_trackgen_roam_sky_path` (~5158) reads level09x `GENSTAMP.TXT` `night=` | the same host writer, or drop the stamp check |
| D3 | The geo slots must stay registered | `td5_trackgen_register_geo_places` (~5040, init ~5260 + the commit); `td5_roam_host_is_geo` uses `td5_trackgen_is_geo_slot`; the GEOSPATIAL entry (`frontend_geo_entry_slot`) needs a registered slot, which today means a ROUTE.JSON from a past BUILD | register a place by its roam tiles (ROAMGRID.JSON) instead of ROUTE.JSON |
| D4 | F11a handoff falls back to a commit output | `td5_roam_handoff_last_route` reads `_route/ROUTE_RAW.JSON` after `routes/geo_map.json`; only `gr_write_route_raw` (td5_geo_route.c ~6061) writes it | drop the fallback (geo_map.json is written by every city race) |
| D5 | Dev roam code reads the derived frame | `td5_roam_view.c` ~247 (derived PLACE.JSON), ~209 `tg_world_geo_h0`; `td5_roam_height_selftest` hosted in `td5_geo_route_init` (`TD5RE_ROAM_HEIGHT_TEST`, uses `td5_geo_sync`, `_place_path`, `_derived_path`) | move the selftest host, re-point the viewer at the source frame |
| D6 | `td5_geo_sync` calls into the route module | `td5_geo_derived_migrate` (td5_geo.c ~890 -> td5_geo_route.c ~5724) | keep the migrate or inline it |
| D7 | The geo map screen is also the roam entry | FREE ROAM + RACE ROUTE live on it, roam exits back to it (td5_game.c ~1704) | keep `Screen_GeoGenerator`, `td5_geo_tiles.c`, the place outlines (`td5_geo_route_places`, `_place_boundary`) |
| D8 | Roam verify scripts have a strip arm | `verify/roam/f4a_tour.ps1` / `f4c_tour.ps1` `-Strip`, `f5a_ab.ps1`, `f9b_trace_ab.py` compare roam to level091 | retire those arms with the strip |

## 1. STRIP-ONLY: dead once the BUILD is gone

| Module | LOC | Key functions / gate | Callers to stub or delete |
|---|---|---|---|
| td5_geo_attrs.c | 523 | `td5_geo_attrs_prepare` (`!td5_geo_loaded()`), `_race_init`, `_speed_cap_units` | td5_trackgen.c ~3685, td5_tg_streets.c, td5_ai.c ~7228/7241, td5_ai_driver.c ~811/822, td5_ai_traffic.c ~5916, td5_hud.c ~3853/7556 |
| td5_geo_avenues.c | 466 | `td5_geo_avenues_sync`, `td5_geo_avenue_at` | tg_avenue, tg_city, tg_furniture, tg_geo_clear, tg_plazafork, tg_realfork, geo_route |
| td5_geo_forks.c | 274 | `td5_geo_forks_sync`, `_get` | td5_tg_branch.c (`tg_geo_forks_n`, ~321), geo_route |
| td5_tg_geo_street.c | 1280 | crossings / bus stops / footpaths prepare + emit (`!td5_geo_loaded` first) | td5_trackgen.c ~3651-3660, ~4428-4438; tg_streets |
| td5_tg_geo_clear.c | 831 | `tg_geo_clear_prepare` and every query (geo_loaded / `s_ready`) | tg_city ~2320 + the strip building callbacks (~3486/3500), tg_prefab, tg_streets. Roam passes its own callbacks to the building core |
| td5_tg_realfork.c | 1919 | `tg_realfork_enabled`, `tg_realfork_build` | only `tg_geo_walk` (tg_road ~1102) |
| td5_tg_plazafork.c | 1741 | `tg_pf_candidates` (`TD5RE_GEO_FORK_PLAZA` + geo_loaded), `tg_pf_commit` | tg_realfork ~1512/1775; the other `tg_pf_*` queries return "no fork" on synthetic |
| td5_tg_blockloop.c | 1456 | `tg_bl_build` (`TD5RE_GEO_FORK_BLOCKLOOP`, realfork, geo_loaded) | tg_pages ~3948, `tg_bl_reset` tg_branch ~378 |
| td5_tg_avenue.c | 1588 | `tg_geo_avenue_n`, `tg_emit_geo_avenue`, `tg_fork_gap_write`, `tg_realfork_walk_owned`, `tg_av_main_pave_clip` (`TD5RE_GEO_PAVE_STREET_CLIP`, inert on synthetic NV) | trackgen ~4108 and the fork modules |
| td5_fe_geo.c (part) | ~600 of 2160 | `geo_commit_and_register`, `geo_do_build`, the `TD5RE_GEO_AUTOBUILD(_N)` block, `geo_route_is_buildable`, `geo_verdict_text`, the strip status branch, the OPTIONS panel rows other than TIME OF DAY (`k_geo_opts`, `k_geo_sects`, `geo_opt_*`, `geo_draw_panel`) | the screen itself stays (D7) |
| td5_geo_route.c (most) | ~5500 of 6514 | `td5_geo_route_commit` + `_commit_reason`, `gr_write_route_raw`, conditioning (run-off, self-crossing, span budget), `gr_self_test` (`TD5RE_GEO_ROUTE_TEST`). `td5_geo_route_build` only feeds the map polyline + the strip verdict (RACE ROUTE readiness does not use it) | see section 3 for what stays |
| td5_fe_race.c (part) | | `frontend_geo_pick_arm` + its retire calls, the strip branch of `frontend_track_excluded_from_selector`, `td5_geo_draw_route` preview (~8784, `td5_geo_preview_route`), the GEOSPATIAL launch's strip fall-through | `frontend_geo_entry_slot` stays while the entry exists (D3) |
| td5_track.c (part) | ~85 | `xspan_track_is_geo` / `xspan_*` (1730-1815) | auto-on only with a geo route |
| verify/geo_*.ps1 / .py | 67 files | AUTOBUILD: geo_commit_immutable, geo_n2_build, geo_region_screen; ROUTE_TEST: geo_commit_guard; AutoRace on level091: geo_attrs_run, geo_bld_gen, geo_fork_validate, geo_tour, geo_tags_identity `-Arm geo`, ... (the rest analyse their output) | `geo_tags_identity -Arm synthetic` and `geo_level_identity.py` stay for level090 |
| re/tools geo route chain | | geo_condition.py, geo_route.py, geo_forks.py (strip route) | geo_fetch.py / geo_region.py probably feed roam's source frame (NV) |

## 2. SHARED WITH SYNTHETIC: keep the module, trim its geo islands

| Module | LOC | Geo-only islands (approx. lines) | Note |
|---|---|---|---|
| td5_tg_network.c | 3858 | `tg_geo_*` 806-1303, `tg_net_geo_*` 1374-2885 (`tg_net_geo_active` ~2784) | synthetic side streets + `td5_trackgen_street_edge_*` (minimap) stay |
| td5_tg_streets.c | 7594 | `tg_geo_area_here` ~2613 .. `tg_geo_plaza_report_impl` ~5122 (geo lawns, paths, parks, plazas) | `tg_geop_lawn_build` is cited in a roam comment only |
| td5_tg_road.c | 2383 | `tg_road_geo_struct_from_data` ~453, `tg_geo_plaza_floor*` ~1013-1068, `tg_geo_walk` ~1087 | the synthetic centreline stays |
| td5_tg_branch.c | 1928 | `tg_geo_forks_n/fork_is_real/kind/at` ~37-80, ~321 | synthetic forks stay |
| td5_tg_bridge.c | 3830 | `tg_up_geo_struct_from_data` ~207 | |
| td5_tg_furniture.c | 1886 | `tg_geo_urban_rails_off` ~99, `tg_geo_signals_prepare` ~1634, `tg_emit_geo_signals` ~1749, report ~1837, finish gate ~1023 | |
| td5_tg_guard.c | 1997 | 3 `td5_geo_loaded` uses (NV) | |
| td5_tg_pages.c | 4304 | 1 `TD5RE_GEO_` knob | writes the level dirs: D1 |
| td5_tg_prefab.c | 490 | `TD5RE_GEO_PREFAB_*` 444-490 | roam reaches `tg_prefab_fit/write_at/half_*` through `tg_geo_emit_landmark_prefab`: keep those |
| td5_trackgen_stream.c | 352 | 2 `td5_geo_loaded` uses (NV) | |
| td5_trackgen_preview.c | 224 | none | studio preview |
| td5_minimap_streets.c | 688 | geo branch: route nodes ~109, `td5_geo_roads_*` ~521-565, slug ~626 | auto branch (~590-616) serves level090; roam has its own minimap |
| td5_tg_terrain.c | 6090 | `tg_biome_geo_override` ~724, `tg_road_page_for_geo_surface` ~1172 | roam uses `tg_prop_slot`, `tg_tree_slot`, `k_biomes[]` (section 3) |
| td5_tg_world.c | 761 | geo_loaded branches (e.g. `tg_world_water_y` ~325) | roam dev viewer uses `tg_world_geo_h0` (D5) |

## 3. SHARED WITH FREE ROAM: keep (or move into a roam-owned module)

| Module | Roam uses (keep) | Strip-only parts (delete) |
|---|---|---|
| td5_geo.c | `td5_geo_source_path` (~60 roam call sites), `_derived_path`, `_place_path`, `_places_rescan/_count/_slug`, `_loaded`, `_place_slug`, `_height`, `_is_water`, `_sync` | the route loader (~479-790, `td5_geo_route_count/node`, xsep), `td5_geo_preview_route`, `td5_geo_select`, `td5_geo_force_place` / `_force_synthetic` (go with the geo slots) |
| td5_geo_roads.c | `_slurp`, `_pavement_default_m`, `_units_per_metre`, `_parse_points`, `_parse_record` (roam_cache, roam_scenery, roam_junction, roam_ribbon, roam_world) | `_sync`, `_get/_point/_nearest`, `_pavement_at`, `_name*` |
| td5_geo_sidewalk.c | all of it (roam_cache, roam_world, roam_tile, roam_scenery) | none |
| td5_geo_signals.c | the lamp clock: `_set_session_time`, `_lamp_phase_now`, `_phase_at`, `_lamp/_cross_colour`, `_countdown` (roam_furniture, roam_traffic, roam_cops, render_mesh) | data: `_signals_sync`, `_get`, `_crossing_get`, `_bus_stop_get` |
| td5_geo_footways.c | `_kind_of`, `_surface_of` (roam_scenery) | `_sync`, `_get`, `_nearest`, `_sidewalk_nearest`, `_footway_sidewalk_near` |
| td5_geo_buildings.c | ring helpers, `_triangulate`, `_parse_building/_area`, `_storey_m_of`, `_area_is_plaza`, `_storey_units` (roam_scenery) | `_sync`, `_building/_area/_ring`, rehost / bind / span, census, `_place_has_extra` |
| td5_geo_landmarks.c | `td5_geolm_apron` (via the building core) | forecourts, `_cluster`, `_kind/_is_anchor/_vetoed/_stats` |
| td5_geo_route.c | `gr_raster_read_window` (roam_height, roam_scenery), `gr_resample` (roam_ribbon), `GrRaster` (roam_height.h), `td5_geo_derived_migrate` (D6), the selftest host (D5), `td5_geo_route_places` / `_place_boundary` (map outlines, D7) | section 1 |
| td5_geo_tiles.c | all of it (the geo map, D7) | none |
| td5_trackgen.c | `td5_trackgen_is_geo_slot`, `_roam_host_slot`, `_roam_pages`, `_roam_is_night`, `_roam_sky_path`, `tg_buf_free`, `_register_geo_places`, `_geo_index_for_slot/_slot_for_index` | `tg_geo_apply_spec`, `tg_geo_track_name`, `tg_geo_sky_profile`, `tg_rolls_geo_requested`, the geo prepare calls ~3641-3686, emit calls ~4108 / ~4413-4438, the geo branch of `_set_target_slot`, `_geo_own_selector` |
| td5_tg_city.c | `tg_emit_box_mesh`, `tg_emit_billboard_mesh`, `tg_write_prefab_mesh`, `tg_write_quad_mesh`, `tg_city_sidewalk_w`, `tg_city_kerb_h`, the building core `tg_geo_emit_building_core` + helpers (~2538-3123: `tg_geo_ring_*`, roof ridge, `tg_geo_emit_landmark_prefab`, `tg_geo_emit_apron`), td5_tg_geo_bld.h | `tg_geo_city_build_begin` (`s_geo_city`), ~1773-2302, `tg_geo_city_prepare`, the strip callbacks `tg_strip_bld_*`, `tg_geo_emit_one`, `tg_geo_emit_buildings`, reports |
| td5_tg_mastarm.c | `tg_plate_upper`, `tg_plate_plan`, `tg_plate_split2` (roam_furniture) | `tg_mastarm_enabled`, prepare / plan / emit / report |
| td5_tg_terrain.c | `tg_prop_slot`, `tg_tree_slot`, `k_biomes[]` | see section 2 |
| td5_roam_handoff.c (F11a) | all, minus the ROUTE_RAW fallback (D4) and `td5_roam_strip_build_enabled` once the knob goes | |

## 4. Roam's include / call surface (for a header split)

Includes: roam_cache (trackgen.h, geo_roads.h, geo_sidewalk.h); roam_cops,
roam_traffic (geo_signals.h); roam_drive, roam_junction, roam_partido,
roam_physics, roam_route_ui (geo_roads.h); roam_furniture (geo_roads,
geo_signals, trackgen, trackgen_internal); roam_mode (trackgen.h); roam_ribbon
(geo_roads, geo_route); roam_scenery (tg_geo_bld, geo_buildings, geo_landmarks,
geo_footways, geo_roads, geo_route, geo_sidewalk); roam_tile (geo_roads,
geo_sidewalk); roam_view (tg_world.h); roam_world (geo_sidewalk); headers
roam_cache.h / roam_world.h (geo_roads.h), roam_height.h (geo_route.h);
roam_handoff (geo.h).

## 5. Suggested F11b order

1. Close D1-D3 first (host writer + slot registration by roam tiles), prove roam
   still looks the same with level091 deleted.
2. Delete section 1 bottom-up (fork modules, attrs / avenues / forks readers,
   geo_street / geo_clear), stubbing the AI / HUD callers of td5_geo_attrs.
3. Trim section 2 islands, keeping the synthetic gate (MODELS E2F1F33C...) green
   after each module.
4. Split section 3 modules so the roam-used halves live in roam-owned files.
5. Retire the strip arms of the verify scripts (D8) and `TD5RE_GEO_STRIP_BUILD`.
