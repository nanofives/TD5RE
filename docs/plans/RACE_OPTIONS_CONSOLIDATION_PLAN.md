# Unified dynamic RACE OPTIONS — retire GAME OPTIONS

## Context

Race-behavior options are split today between the GAME OPTIONS screen (screen 13, reached from the OPTIONS hub) and the newer RACE OPTIONS screen (screen 44, reached from track-select). The goal: **GAME OPTIONS disappears**; every option it held becomes available as RACE OPTIONS in **every game mode**, reachable from each mode's track-selection screen — and modes with no track select (SP/MP Drag, faithful-mode Cup) get RACE OPTIONS inserted as a pre-launch frontend step. Defaults stay shared across modes (they already live in `g_td5.ini.*` → `td5re.ini`), and the system is dynamic: each mode shows only the rows it needs.

User decisions (asked & answered):
- **PLAYER NAME** → promoted to its own row on the OPTIONS hub (not into RACE OPTIONS).
- **Netplay** → host edits RACE OPTIONS; values broadcast to clients (extend `TD5_NetRaceConfig`, same-build assumption per existing precedent for dynamics/mode_config).

## Architecture

Reuse `Screen_RaceOptions` (`td5_fe_race.c:6756`) + the `td5_raceopts_*` model (`td5_frontend.c:6385-6488`) as the single options surface. Its infrastructure already provides: return-to-caller (`s_raceopts_parent`), commit-on-any-exit (`raceopts_leave()` → `raceopts_commit_persist()` → `g_td5.ini.*` + `td5_ini_persist_options()`), and per-mode row hiding. We add: the 5 missing rows, a data-driven availability matrix + pagination over a filtered row list, a `raceopts_open(parent, launch_after)` entry helper, netplay replication, then delete GAME OPTIONS.

## Slices (each buildable; do 1+2 as one commit — 13 rows overflow the canvas without pagination)

### Slice 1 — Absorb missing GAME OPTIONS rows into the RACE OPTIONS model
- Extend `RO_*` enum (`td5_frontend_internal.h:601-604`) with `RO_COLLISIONS, RO_DAMAGE, RO_LANEASSIST, RO_TUTORIAL`.
- Extend `td5_raceopts_label/value/cycle` (`td5_frontend.c:6385-6488`) by porting the matching `GO_*` cases from `td5_gameopts_value/cycle` (`td5_frontend.c:6229-6333`).
- Extend `raceopts_commit_persist()` (`td5_fe_race.c:6716`) with the remaining `td5_gameopts_commit()` body (`td5_frontend.c:6344-6373`): `ini.collisions`, `ini.car_damage`+`car_damage_bar`, `ini.lane_assist`, `ini.tutorial_overlay` (preserve dev value ≥2). Verify launch path still applies collisions (`td5_frontend.c:~4514`) — no double-apply.
- Difficulty reconciliation: `RO_DIFFICULTY` keeps editing per-race `s_race_difficulty`, but commit also writes `g_td5.ini.difficulty` + `s_game_option_difficulty` (it becomes the only difficulty control). Seeding unchanged (`td5_fe_race.c:6930`).

### Slice 2 — Dynamic availability + pagination (core mechanism)
- New `TD5_RaceOptsCtx` (game_type, is_cup, is_quick_race, is_mp, mp_mode, is_net, is_drag, opponents) + `td5_raceopts_row_available(ro, ctx)` in `td5_frontend.c`, replacing the ad-hoc hides at `td5_fe_race.c:6766-6773` (fold in `frontend_update_police/difficulty_button_visibility` logic).
- `td5_raceopts_build_rows(ctx)` fills a compact filtered row list; pagination mirrors the GAME OPTIONS mechanism (`GO_ROWS_PER_PAGE 7`, PREV/NEXT only when >7 visible rows; `td5_frontend.c:6161-6208` as pattern). Hidden rows are omitted, not blanked.
- Rework `Screen_RaceOptions` case 0 (build ctx + page) and case 6 (button→option map, PREV/NEXT rebuild, OPPONENTS change rebuilds list); render dispatch at `td5_frontend.c:9885-9889` loops the filtered list (values at x=350, arrows via `fe_draw_option_arrows`).

**Availability matrix** (Y = shown; flip is one line in the switch). SP=single, TT=time trial, CC=cop chase, DR=SP drag, CUP=SP cup, QR=quick race; MP: MPR race, MPC cup, TB battle, MCC cop chase, MDR drag; NET=netplay host.

| Row | SP | TT | CC | DR | CUP | QR | MPR | MPC | TB | MCC | MDR | NET |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| OPPONENTS | Y | Y | Y | – | – | Y | Y | – | – | Y | – | Y (non-drag/TB) |
| TRAFFIC | Y | Y | Y | – | Y | Y | Y | Y | Y | Y | – | Y |
| POLICE | Y | Y | – | – | Y | Y | Y | Y | Y | – | – | Y (not cop mode) |
| DIFFICULTY | Y° | Y° | Y° | Y | – | – | Y° | – | – | – | Y | Y° |
| DYNAMICS | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y |
| CHECKPOINTS | Y | Y | Y | – | Y | Y | Y | Y | – | Y | – | Y |
| POWER-UPS | Y | Y | Y | – | Y | Y | Y | Y | Y | Y | – | Y |
| TOUGHNESS/DEFORM/COLLISIONS/DAMAGE | Y everywhere |
| LANE ASSIST | Y | Y | Y | Y | Y | Y | – | – | – | – | – | Y |
| TUTORIAL | Y | Y | Y | Y | Y | Y | – | – | – | – | – | – |

° existing rule: hidden at 0 opponents / Quick Race / MP cop chase. Drag hides TRAFFIC (own `DragTraffic` knob, `main.c:1039`). MP hides LANE ASSIST (per-player on MP profile) and TUTORIAL. Uncertain cells (CHECKPOINTS in TB/CC, POWER-UPS in TT) get confirmed against `td5_game.c` mode setup during implementation.

### Slice 3 — PLAYER NAME → OPTIONS hub row
- Add a "PLAYER NAME" row to `Screen_OptionsHub` (`td5_fe_menu.c:1436-1518`) in the slot GAME OPTIONS vacates; Enter-to-edit reusing the existing editor `td5_gameopts_name_edit_begin/tick` (`td5_frontend.c:6502-6528`), renamed `td5_playername_edit_*` (it self-persists `[GameOptions] PlayerName` on Enter — no commit plumbing needed). Show current name as the row's value text (x=350).

### Slice 4 — Entry points for modes without track select
Generalize entry: `raceopts_open(int parent_screen, int launch_after)`; `raceopts_leave()` splits — OK with `launch_after` → commit + `frontend_init_race_schedule()` + `frontend_init_display_mode_state()`; BACK/ESC → commit + return to parent. Screen 44 itself is the "new frontend step", styled identically everywhere. Call sites (all `td5_fe_race.c`):
1. **SP Drag** — car-select pass-2 OK (`6492-6511`): `raceopts_open(CAR_SELECTION, 1)` instead of launching. BACK re-enters opponent pass (`s_drag_carselect_pass=1`, reseed cursor from `s_p2_car/s_p2_paint` like `6481-6486`).
2. **MP Drag** — the TrackSelection init chokepoint that pins the strip + launches (`6842-6849`): route through `raceopts_open(<caller>, 1)`; guard against re-entry loop from BACK.
3. **Faithful SP Cup** (knob off, `6449-6461`): only at `s_race_within_series == 0`, `raceopts_open(CAR_SELECTION, 1)` after schedule track resolves.

### Slice 5 — Netplay replication (host sets, sync to all)
- Append to `TD5_NetRaceConfig` (`td5_net.h:110-144`): `powerups, car_toughness, car_deform, car_damage, collisions, checkpoint_timers` (struct is memcpy'd wholesale into DXPSTART — no wire code; same-build assumption per existing precedent; bump protocol version constant if one exists in `td5_net.c`).
- Host fill at `td5_fe_net.c:2035-2082` (next to existing traffic/cops/dynamics lines); client apply at the existing consume points (`td5_game.c:2109-2146` / `2431`) into the live in-memory fields **without** calling `td5_ini_persist_options()` (mirror exactly how traffic is applied — client's INI must not be overwritten).
- LANE ASSIST / TUTORIAL / PLAYER NAME stay local-only.

### Slice 6 — Remove GAME OPTIONS (last, so nothing regresses mid-way)
- `Screen_OptionsHub`: GAME OPTIONS row replaced by PLAYER NAME (slice 3), cases renumbered.
- Delete `Screen_GameOptions` (`td5_fe_menu.c:1520-1637`), NULL slot 13 in the screen table (`td5_frontend.c:87-144`), add `TD5_SCREEN_GAME_OPTIONS` to the retired-slot redirect in `td5_frontend_set_screen` (`td5_frontend.c:4756-4766`).
- Remove the back-handler commit hook (`td5_frontend.c:4025-4026`); delete the `td5_gameopts_*` model (`td5_frontend.c:6161-6373`, keep the renamed name editor) + internal.h decls; grep `td5_gameopts_` tree-wide.
- Selftest: drop `scr-game-options` (`td5_selftest.c:173`, per 2026-07-03 retired-screen precedent); add `scr-race-options` if screen 44 tolerates context-free entry.
- `inputscripts/navigate_game_options.txt` → rewrite as `navigate_race_options.txt`; check other scripts for screen 13.
- Docs: `FRONTEND_SCREEN_GUIDE.md`, `td5_changelog.h`, `pending_to_test.csv` (retire stale "Game Options:" rows, add RACE OPTIONS per-mode rows), stale screen-list comments (`td5re.h:608`, `td5_fe_menu.c:7`).

## Critical files
- `td5mod/src/td5re/td5_fe_race.c` — Screen_RaceOptions, raceopts_open/leave/commit, drag/cup/MP-drag entry points
- `td5mod/src/td5re/td5_frontend.c` — td5_raceopts_* model, availability+pagination, render dispatch, screen table, retired-slot redirect, back handler
- `td5mod/src/td5re/td5_frontend_internal.h` — RO_* enum, ctx struct, model API
- `td5mod/src/td5re/td5_fe_menu.c` — OptionsHub PLAYER NAME row, Screen_GameOptions deletion
- `td5mod/src/td5re/td5_net.h`, `td5_fe_net.c`, `td5_game.c` — net config extension, host fill, client apply
- `td5mod/src/td5re/td5_selftest.c`, `FRONTEND_SCREEN_GUIDE.md`, `td5_changelog.h`, `pending_to_test.csv`

## Verification
1. `build_all.bat` clean after every slice (structure lint report-only locally, gates CI).
2. Selftest screen walk passes with `scr-game-options` removed (+ `scr-race-options` green if added). Golden race traces must be byte-identical — no default option values change in this plan.
3. Manual matrix walk: for each of the 12 modes open RACE OPTIONS, confirm exactly the matrix rows, cycling works, PREV/NEXT + PAGE x/y appear only when >7 rows, OK/BACK persist to `td5re.ini` `[GameOptions]`.
4. New entry points: SP Drag (options → OK launches; BACK returns to opponent pass with cars preserved — regression-sensitive, see `td5_fe_race.c:6474-6503` s_p1/s_p2 comments), MP Drag (appears once, no loop), faithful cup (only before race 1).
5. Netplay 2-instance run: host sets POWER-UPS=CHAOS + COLLISIONS off + TOUGHNESS OFF vs client INI set opposite — no desync, matching boxes/damage on both, client INI untouched.
6. Regressions: OPTIONS hub nav with PLAYER NAME row, ESC-from-RACE-OPTIONS persists, Quick Race flow, cup picker (screen 43) still hides OPPONENTS/DIFFICULTY.
