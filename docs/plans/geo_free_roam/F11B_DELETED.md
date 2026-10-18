# F11b: what was deleted, what stayed and why (2026-10-18)

Mariano's decision (2026-10-18): the strip geo race stays ONLY for local
multiplayer (2+ humans on one machine). Free roam hosts itself (B2), the place
registers without a strip BUILD (B3), and the strip-only code the multiplayer
strip race does not use is deleted. Branch roam-F11b, base integ-roam @ 8b717824.

## The finding that sizes this list

The multiplayer strip race IS the strip BUILD: the same commit
(`td5_geo_route_build` + `td5_geo_route_commit`), the same trackgen pipeline,
and the same fork / avenue / block-loop / plaza / signal / street modules,
proven byte-identical below. In local multiplayer the geo map shows BUILD
TRACK and the OPTIONS panel again, and that panel can switch FORKS, BLOCK
LOOPS, LEFT LOOPS, PLAZA FORKS, LEFT FORKS, AVENUES, RUN-OFF, SIGNALS, MAST
ARMS, CROSSWALKS, BUS STOPS, FOOTPATHS, CITY, BUILDINGS, PLAZAS, PARK TREES
and STREET LAMPS, so the OFF branch of every one of those knobs is reachable
too. So sections 1 and 2 of F11_REMOVAL_MAP.md (section 1 alone is about
16 100 lines by that map's LOC column: td5_geo_attrs / _avenues / _forks,
td5_tg_geo_street / _geo_clear / _realfork / _plazafork / _blockloop /
_avenue, ~5 500 of td5_geo_route.c, ~600 of td5_fe_geo.c; plus the geo islands
of the shared modules) are **used by the multiplayer race and stay**. What could go
is code that neither the defaults nor the panel reach, plus the
single-player-only paths.

## Deleted: 236 lines in 18 places (`_archive/roam/F11b/ledger.json`, written by cut.py)

| File | Lines | What | Why it was dead |
|---|---|---|---|
| td5_geo_route.c | 30 | `td5_geo_route_drop_graph`, `_place_ring_count`, `_snap`, `_last` | no caller anywhere (C, verify/, scripts/) |
| td5_geo_route.h | 13 | their prototypes + the snap API comment | as above (the snap radius `TD5_GEO_ROUTE_SNAP_MAX_M` stays, the router uses it) |
| td5_geo.c / .h | 6 | `td5_geo_route_name_count`, `td5_geo_preview_count` | no caller |
| td5_tg_blockloop.c | 29 | `TD5RE_GEO_BLOCKLOOP_ONLY` (`bl_only_ok`), `TD5RE_GEO_BLOCKLOOP_LEFT_ONEWAY` | dev filter / refusal, default off, no script sets them |
| td5_tg_plazafork.c | 14 | `TD5RE_GEO_PLAZA_DUMP` (log/pf_chain_*.csv) | dump nobody reads, default off |
| td5_tg_realfork.c | 20 | `TD5RE_GEO_FORK_LEFT_CONTRA`, `_RELAX_ONLY`, `_TAPER_SMOOTH` (+ its comment), `_NODEDUMP` | default off, no user; TAPER_SMOOTH was measured no better than linear |
| td5_tg_avenue.c | 4 | `TD5RE_GEO_AVENUE_EXIT_SLACK` / `_ENTRY_SLACK` | they brought back two fixed bugs (rounds 1021 A / 1012), default off |
| td5_tg_network.c | 6 | `TD5RE_GEO_NET_SYNTH` / `_NET_SYNTH_ONLY` | synthetic streets on a real place, default off |
| td5_tg_city.c | 3 | `TD5RE_GEO_SW_SIDE_FLIP` | a settled A/B, default off |
| td5_geo_roads.c | 16 | `TD5RE_GEO_SW_FROM_WIDTH` | speculative pavement from `width`, default off |
| td5_fe_race.c, td5_frontend.c, td5_trackgen.c/.h | 16 | `TD5RE_GEO_OWN_SELECTOR=0` (the pre-1026 layout: a place listed only after BUILD armed it) | single-player BUILD flow; single player has no BUILD since F11a |
| inputscripts/ | 79 | `n2_geo_build_exit`, `_race`, `_twice`, `r1026l_qr_geo_build`, `_geo_race` | single-player strip BUILD / race flows, no user |

Replaced rather than deleted (F11b's own changes):
- the roam sky's level09x `GENSTAMP.TXT` check (`td5_trackgen_roam_sky_path`,
  D2): the sky is now always the pool pick, the same file the strip build
  copies (level029 for La Plata by day, md5-identical);
- the level09x file checks in `td5_trackgen_roam_host_slot` (D1): replaced by
  the roam host writer;
- the handoff's `_route/ROUTE_RAW.JSON` fallback (D4): gone, `routes/geo_map.json`
  is the only saved city route.

## Stayed, and why

| What | Why |
|---|---|
| Every section-1 module and every geo island of section 2 | the multiplayer strip race runs them (the level091 identity runs below) |
| The OPTIONS panel and its rows | shown in local multiplayer (or with `TD5RE_GEO_STRIP_BUILD=1`) |
| `TD5RE_GEO_AUTOBUILD`, `_SEED`, `_SEED_LATLON`, `_PANEL_*`, `_ROUTE_TEST` | dev harnesses of the kept strip BUILD (geo_commit_immutable / geo_n2_build / geo_region_screen / geo_commit_guard); AUTOBUILD now implies the dev strip knob |
| `TD5RE_GEO_STRIP_BUILD` | the dev door to the strip in single player (the level091 identity gate uses it) |
| Diagnostic logs (`FORK_DIAG`, `NET_DIAG`, `AVENUE_DIAG`, `AVENUE_PROBE`, `FRONTAGE_DIAG`, `FARWALK_DIAG`, `BLOCKLOOP_DUMP`) | they serve the strip that stays; several have verify users |
| `TD5RE_GEO_LIT_FORCE_NO`, `_XING_UNKNOWN` | the only way to exercise the NO-lighting branch; roam keeps the same XING_UNKNOWN knob |
| `td5_geo_height_raw_m_units`, `td5_geo_in_bounds` | dead, but generic place sampling, not strip code (out of scope) |
| verify/geo_*.ps1 strip tools, verify/roam/f4a, f4c `-Strip`, f5a_ab `-Geo` | they test the strip that stays. The .ps1 ones cannot be edited on this account: run their single-player AutoRace arms with `-Extra @{TD5RE_GEO_STRIP_BUILD="1"}` where they take `-Extra`. f5a_ab.ps1 `-Geo` clears TD5RE_* and now races the city (needs an edit with the knob). verify/roam/f9b_trace_ab.py passes the knob itself |

## Evidence (all in `_archive/roam/F11b/`)

- level091, DEV path (AutoRace slot 61, `TD5RE_GEO_STRIP_BUILD=1`, seed 2054677019) new exe vs main's td5re.exe: bytes IDENTICAL, MODELS md5 A874E2E0 (`id_geo.txt` before the deletions, `id_geo2.txt` after).
- level091, MULTIPLAYER path on a fresh copy (no level091, no `_route/`): `--AutoRace=1 --DefaultPlayers=2` built the strip on demand from the 4 pins in 4.35 s, then a split-screen race; main's exe BUILDing the same 4 pins from the geo map (AUTOBUILD): bytes IDENTICAL (`id_geo_mp.txt`, `id_geo_mp2.txt` after the deletions). The derived `_route/` is identical file for file except ROUTE_RAW.JSON's `waypoints` (8-decimal pins from geo_map.json vs the map's doubles).
- synthetic level090 (SELECTED.TXT aside): bytes IDENTICAL, MODELS sha256 E2F1F33C (`id_syn.txt`, `id_syn2.txt`).
