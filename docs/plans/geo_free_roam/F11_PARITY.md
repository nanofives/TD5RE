# F11 parity checklist: strip geo race vs free-roam race (F11a, 2026-10-18)

Base integ-roam @ f5940f1c (F0-F10 + F8e), branch roam-F11a. Decision under test
(GEO_FREE_ROAM.md): "Old strip geo BUILD: kept until the live-wall race reaches
parity, then retired". Mariano signs off.

Sources: **[M]** measured in this task (command + log in `_archive/roam/F11a/`),
**[S]** read in the source (file:line at f5940f1c), **[R]** an earlier round's
report or memory note (named), **NV** not verified.

Verdicts: **parity**, **roam better**, **roam missing (BLOCKER)**, **roam missing
(minor)**.

## Blockers first

| # | Capability | Strip | Free roam | Verdict |
|---|---|---|---|---|
| B1 | **Local split-screen (2+ humans)** | Not blocked by code: Quick Race / single race with 2..9 humans can pick the GEOSPATIAL entry. No selftest row covers a geo track in split screen (NV that it plays well) | **Single player only**, hard-coded in 5 places: `frontend_roam_prepare_car_select` (td5_fe_race.c, humans 1, no MP flow), `td5_roam_apply_overrides` (td5_roam_mode.c: `num_human_players 1`, `split_screen_mode 0`, `num_spectate_screens 0`), world / barrier draw on pane 0 only (td5_game.c ~8594), slot-0-only input filter (td5_input.c ~989), the race / AI grid assume the player is slot 0 [S] | **roam missing (BLOCKER)**. F11a leaves the strip race on the GEOSPATIAL entry when humans > 1 |
| B2 | **Roam's look depends on a strip build** | The BUILD writes level091 (TEXTURES.DAT, LEVELINF.DAT, FORWSKY.png, GENSTAMP.TXT) | `td5_trackgen_roam_host_slot` (td5_trackgen.c ~5106) uses level091 as the roam host only if those 3 files exist, else falls back to a shipped level (Moscow pages); `td5_trackgen_roam_sky_path` reads GENSTAMP.TXT `night=` [S] | **BLOCKER for F11b (deletion)**, not for the UI change: level091 must still be written by something (a host-only writer or shipped files) |
| B3 | GEOSPATIAL entry needs a registered geo slot | The entry is listed only when `td5_trackgen_register_geo_places` registers a place, which needs a ROUTE.JSON from a past BUILD (td5_fe_race.c `frontend_geo_entry_slot`) [S] | Quick Race's city race is reached through that entry | **BLOCKER for F11b**: a fresh install without a past BUILD has no GEOSPATIAL entry. Today la_plata_partido ships `_route/ROUTE.JSON` |

## Everything else

| Capability | Strip | Free roam | Verdict |
|---|---|---|---|
| Route building: waypoints | 32 max (`GEO_MAX_PTS`, td5_fe_geo.c) [S] | 24 max (`TD5_ROUTE_MAX_WP`) [S]. RACE ROUTE greys out over 24 with AT MOST 24 POINTS IN THE CITY [M] | roam missing (minor): raise TD5_ROUTE_MAX_WP if 25-32 pins matter |
| Route building: A* | one A* per leg over ROADS.JSON [S] | directed A* over the planner graph, one-way respected, no U-turn except at a dead end [S]. Same 4 pins: strip 7.1 km preview / 2042 spans, roam 7483.6 m / 69 edges [M] | parity |
| Run-off past the finish | 104 spans default, 0..200 option [S] | not needed: the FINISHED car brakes to a stop, closing barrier past FINISH [S] | parity (different design) |
| Self-crossing routes | a level crossing rejects the route [S] | allowed (live walls, figure-8 census) [S][R F9a] | roam better |
| Max route length | 3000 spans ≈ 10.5 km incl. grid + run-off [S] | no span cap; 27.4 km Plaza Moreno -> City Bell -> Plaza Moreno, 16 rebases, 0 misses [R F9b] | roam better |
| Forks / corridors / block loops / plaza forks | only accepted forks, cap `TD5_TG_BRANCH_MAX` 20; side streets are scenery [S] | every street drivable; in a race every mouth the route does not take is barred. The 7.48 km route: 225 barriers (203 mouth, 22 turn), 65 clusters, 0 open arms, 0 across route [M] | roam better |
| Signals + mast arms + plates | drawn, live countdown [S] | drawn, live cycle; 308 clusters, 616 near + 448 far mast arms, 114 plate names, 0 dropped [R F4c/F4f] | parity |
| Signals obeyed | nobody (no AI reads `td5_geo_signal_*`) [S] | traffic and cops obey them; 0 red violations of 105 in a 10-min soak [R F8]. Race opponents ignore them (by design, like strip) | roam better |
| Sidewalks / buildings / landmarks | full set | buildings, landmarks, sidewalks, kerbs, railings, footpaths, plaza lawns + trees, furniture [R F4a-F4f]. Flora rows (F4d) not done, ground is a plain grid; water / far terrain NV | roam missing (minor) |
| AI quality | normal TD5 AI on the strip; fork harness PASS on F=89 / F=790, 0 stall / spin / jam [R round 1028 S]. **No finish rate or lap time on a geo track exists anywhere** | 5/5 finish, 0 stuck on the 3.11 km roam-sprint-ai; 3 AI x 3 laps of 1.5 km, laps 109.96 / 109.50 / 109.53 s, 0 respawns [R F8c]. 7.48 km route: 3 and 5 opponents started and raced [M], finish NV (runs were 30 s) | roam measured, strip not: like-for-like NV. Opponents: roam 5 max vs strip up to 9 (roam missing, minor) |
| Traffic | standard TRAFFIC option, not geo-aware (lanes / turns NV on level091) [S] | lane follower on the graph, turns at junctions, obeys signals, OFF / LOW / HIGH per race, 12-14 cars live in a 10-min soak, 0 off-surface [R F8] | roam better |
| Cops | POLICE option / Cop Chase not gated on geo, NV on level091 [S] | patrol, pursuit, heat 1..5, bust; race COPS row, default OFF [R F10] | parity (roam has no Cop Chase *mode*, see modes) |
| **Load time to first frame, same 7.48 km route** | Quick Race GEOSPATIAL with `TD5RE_GEO_STRIP_BUILD=1`, fresh level091: generation 3.7 s, ready in **3814 ms**, then the grid is held **11.15 s** waiting for scenery (1539 frames) before the 3-2-1 [M `out/qr_strip`, `hold_time.py`] | Quick Race GEOSPATIAL (city): first frame **3076 ms** after the session began, route planted 178 ms later, race started **288 ms** after the first frame [M `out/qr_city`]. Geo map RACE ROUTE: first frame 3711 ms [M `out/geo_race`]; SELECT TRACK GEOSPATIAL right after the gates rebuilt level091: first frame 6523 ms, race started 768 ms later [M `out/trksel_city`]. So 3.1-6.5 s to the first frame across 3 runs | **roam better**: ~3.4 s vs ~15 s to the countdown |
| Laps / circuits | none: a geo route is always point-to-point (`circuit=0`, td5_trackgen.c ~5054) [S] | SPRINT / CIRCUIT, laps 1..10 on the setup panel [S][R F9a] | roam better |
| Saved routes | one ROUTE.JSON per place, overwritten by every BUILD [S] | by name in routes/, list / load / delete, MAP CHANGED check, best time + best lap per route. F11a saves every handed-over route as `geo_map` [M] | roam better |
| Netplay | GEOSPATIAL entry never listed in a network lobby (td5_fe_race.c ~784) [S] | forced off [S] | parity (both missing) |
| Game modes | single race; Cop Chase / time trial / chaos co-op not gated on geo (NV that they work on level091) | free roam, sprint, circuit, cops; time trial / drag / wanted / chaos forced off [S] | roam missing (minor): no Cop Chase / time trial on a real city |
| Replays | recorder not gated (View Replay on level091 NV) | no replay (buffer discarded, F8b) [R F8b] | roam missing (minor) |
| Weather | from the level's zones (rain on level091 NV) | none: the zone update is skipped in roam [S] | roam missing (minor) |
| Time of day | TIME OF DAY row (AUTO / DAY / NIGHT) | the same row: `td5_trackgen_roam_is_night` reads `TD5RE_AUTOTRACK_NIGHT` [S]. F11a keeps that one row on the city panel [M PNG] | parity |
| Minimap | geo streets + labels | own street map + the route [S]; cop markers open | parity |
| Results / best times | normal results screen, no high scores for geo | own results panel, splits, best per route [S] | roam better |
| Selftest / identity coverage | **0 selftest rows** on a geo track; the level091 identity gate (`verify/geo_tags_identity.ps1 -Arm geo`) and the fork harness are the only nets | 15 roam rows (smoke: roam, roam-sprint-smoke, roam-sprint-ai-smoke, roam-circuit-smoke, roam-traffic-smoke, roam-cops-smoke; full: roam-corners, roam-sprint, roam-sprint-ai, roam-circuit, roam-traffic, roam-sprint-traffic, roam-cops, roam-ram, roam-partido) [S] | roam better. The level091 gate keeps running with `TD5RE_GEO_STRIP_BUILD=1` |
| Quick Race / SELECT TRACK entry | GEOSPATIAL entry + SETTINGS chip (opens the geo map) | F11a: the same entry launches the city race (1 human): Quick Race [M `out/qr_city`, 5 AI], SELECT TRACK [M `out/trksel_city`, 5 AI]; the geo map's RACE ROUTE [M `out/geo_race`, 3 AI]. With `TD5RE_GEO_STRIP_BUILD=1` Quick Race's entry runs the strip race again [M `out/qr_strip`]. 2+ humans fall back to the strip by code (`frontend_geo_entry_launch_roam`), not run | parity after F11a |
| Places | any registered geo place | needs `roam/ROAMGRID.JSON` + tiles; only la_plata_partido has them [S] | roam missing (minor): build tiles per place |
| Force feedback / damage | normal | not gated (NV); every roam row runs with damage off; softer crash threshold in roam (F8e) | NV |

## What F11a retired from the UI (items with parity)

- Geo map: **BUILD TRACK -> RACE ROUTE** (the same pins raced in the city: car
  select, then the SPRINT RACE setup panel over the route map). The OPTIONS panel
  shrinks to TIME OF DAY (read by the city session). HILLS
  (`TD5RE_GEO_EXAGGERATION`), PLAZAS and FOOTPATHS are also read by roam tile /
  render code, but whether a change reaches already-cached ROAMTILE files is NV,
  so they stay off the city panel until that is checked. Status box: READY TO RACE / CANNOT RACE
  with the reason (pin outside the city map, more than 24 pins).
- Quick Race and SELECT TRACK (single race), GEOSPATIAL entry, **one human**:
  the city race starts straight away (AUTOSTART) with the menus' opponents
  (capped at 5), traffic (OFF / LOW / HIGH from the volume) and police setting,
  on the geo map's pins, else `routes/geo_map.json`, else the last strip
  BUILD's `_route/ROUTE_RAW.JSON`.
- **Left on the strip** (no parity): 2+ humans on the GEOSPATIAL entry (B1),
  cup / time trial / cop chase / drag types, network.
- **The strip BUILD stays reachable** with `TD5RE_GEO_STRIP_BUILD=1` (read in
  dev and release builds): BUILD TRACK, the full OPTIONS panel, the strip race
  from every GEOSPATIAL entry. `TD5RE_GEO_AUTOBUILD` (dev harness) still presses
  the strip BUILD regardless.

## Open for Mariano's sign-off

1. B1 split-screen: lift roam to N panes (render, HUD, route UI, per-player race
   state, input) or keep the strip race for local MP after F11b?
2. B2 / B3: what writes level091's host files and registers the GEOSPATIAL
   entry once the BUILD code is deleted?
3. Minor gaps worth closing before F11b: 24 vs 32 pins, 5 vs 9 opponents,
   weather, replays, Cop Chase / time trial on a real city, flora (F4d).

## Gates run for F11a

- level091, `-Seed 2054677019`, `TD5RE_GEO_STRIP_BUILD=1`, new exe vs `td5re_base.exe` (f5940f1c):
  `verify/geo_level_identity.py` says **bytes IDENTICAL** (MODELS md5 A874E2E0..., sha256
  EB199B9E...; STRIP md5 97C89979...; TEXTURES md5 2290C475...) [M `id_geo.txt`].
- Synthetic level090 (SELECTED.TXT aside), base vs new: **bytes IDENTICAL**, MODELS sha256
  E2F1F33C221D61CAB0EFF701484011F0 [M `id_syn.txt`].
