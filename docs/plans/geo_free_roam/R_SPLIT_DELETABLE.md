# Round R (roam-RA): what becomes deletable once split-screen city races ship (2026-10-25)

Base master 1e764f77, branch roam-RA. Nothing is deleted this round (brief rule). Source of the
list: `F11_REMOVAL_MAP.md` section 1, re-measured at this branch (`wc -l`, 2026-10-25).

## Why it is dead now

Until this round the strip geo BUILD had one player-facing user: **local multiplayer** (F11b,
`frontend_geo_strip_context()` sent 2+ humans to the strip built on demand). Round R moves local
split screen onto the free-roam city (SPRINT / CIRCUIT entry, the geo map's RACE ROUTE, AutoRace
`--DefaultPlayers>=2`). `frontend_geo_strip_context()` is now `td5_roam_strip_build_enabled()`
only, so **no default path builds level091 any more**. The strip is reached only through
`TD5RE_GEO_STRIP_BUILD=1` (the level091 identity gate, dev A/B; read in release too).
Network play never listed a geo slot and still does not.

So section 1 of the removal map has no player-facing caller left. Deleting it also retires the
level091 identity gate (`verify/geo_tags_identity.ps1 -Arm geo`), which only exists to guard it.

## Section 1, re-measured (strip-only, deletable)

| Module | Lines now | Note |
|---|---|---|
| td5_geo_attrs.c + .h | 523 + 149 | callers to stub: td5_trackgen.c, td5_tg_streets.c, td5_ai.c, td5_ai_driver.c, td5_ai_traffic.c, td5_hud.c |
| td5_geo_avenues.c + .h | 466 + 91 | readers in tg_avenue, tg_city, tg_furniture, tg_geo_clear, tg_plazafork, tg_realfork, geo_route |
| td5_geo_forks.c + .h | 274 + 80 | td5_tg_branch.c `tg_geo_forks_*` |
| td5_tg_geo_street.c | 1280 | trackgen prepare / emit calls |
| td5_tg_geo_clear.c + .h | 831 + 127 | tg_city strip callbacks, tg_prefab, tg_streets (roam passes its own callbacks to the building core) |
| td5_tg_realfork.c | 1899 | only `tg_geo_walk` (tg_road) |
| td5_tg_plazafork.c | 1727 | tg_realfork |
| td5_tg_blockloop.c | 1427 | tg_pages, `tg_bl_reset` in tg_branch |
| td5_tg_avenue.c | 1584 | trackgen + the fork modules |
| **whole modules** | **10,458** | 10,011 in .c + 447 in headers |
| td5_fe_geo.c (part) | ~600 of 2562 | BUILD TRACK / `geo_commit_and_register` / `geo_do_build` / `TD5RE_GEO_AUTOBUILD` / the strip OPTIONS panel (rows other than TIME OF DAY) / the strip status branch. The screen stays (geo map, RACE ROUTE, FREE ROAM setup) |
| td5_geo_route.c (most) | ~5,500 of 6481 | commit + conditioning + `gr_write_route_raw` + `gr_self_test`. Keep `gr_raster_read_window`, `gr_resample`, `GrRaster`, `td5_geo_derived_migrate`, the place outlines (removal map section 3) |
| td5_fe_race.c (part) | ~120 | `frontend_geo_entry_prepare_strip` (only reached with the knob since round R), the strip branch of `frontend_track_excluded_from_selector`, the strip preview in `td5_geo_draw_route` |
| td5_track.c (part) | ~85 | `xspan_*` localiser (`xspan_track_is_geo`, line 1748 here) |
| **total** | **~16.8k** | the removal map's ~16.1k plus the round-R additions above |

Also strip-only (not counted): 67 `verify/geo_*` scripts (the level091 ones; keep
`geo_tags_identity -Arm synthetic` and `geo_level_identity.py` for level090) and the
`re/tools` strip route chain (geo_condition.py, geo_route.py, geo_forks.py).

## Newly dead by default this round (small, delete with section 1)

- `frontend_geo_strip_context()` itself (now a one-line alias of the knob) and its 9 call sites:
  each becomes `td5_roam_strip_build_enabled()` or drops its strip arm.
- td5_frontend.c AutoRace: the `frontend_geo_strip_context()` branch (strip on demand for
  `DefaultPlayers>=2`) only runs with the knob.
- td5_fe_geo.c `s_strip` screen mode (BUILD TRACK button label, OPTIONS panel visibility).

## Still needed before deleting (unchanged from the removal map)

- D5 dev roam code reading the derived frame (td5_roam_view.c, the height selftest host).
- D6 `td5_geo_derived_migrate` is called from `td5_geo_sync`.
- D8 roam verify scripts with a strip arm (`f4a_tour` / `f4c_tour` `-Strip`, `f5a_ab`, `f9b_trace_ab.py`).
- Section 2 islands (shared with the synthetic level090) are NOT deletable wholesale: trim them
  module by module with the synthetic gate (MODELS E2F1F33C...) green after each.

## Not in scope

The strip code stays this round (brief). Round R's own gate still runs it once:
level091 `-Seed 2054677019` with `TD5RE_GEO_STRIP_BUILD=1`, byte-identical to master.
