# Race options per mode (round UB, 2026-11-08; Mariano's decisions applied in round UE)

Mariano: "on time trials i selected traffic very high and it doesn't get reflected on the race, make sure in every race
mode every valid option is passed correctly". This page is the audit: for every race mode and every race option, is
the option offered, and does the value reach the race. Code read at master 6610a463 plus the round UB fixes.

**How "reaches the race" is checked:** `race.log` has one line per race, written after every per-mode override
(drag, traffic battle, net, LEVELINF traffic flag):

```
RACE OPTIONS USED: gt=0 tt_synth=1 ... laps=1 traffic=1 vol=4 police=1 opponents=5 tier=2 collisions=1 cp_timers=0 circuit=0
```

`td5_game.c` (InitRace, after step 4a). Selftest rows `u5-*` assert it for the modes fixed here.

## Where an option travels

1. **Race type picked** (RACE menu, Quick Race entry, cup select). `ConfigureGameTypeFlags` (`td5_frontend.c`) writes
   the runtime flags from the option values *as they are at that moment* and forces the per-mode ones (the original's
   `ConfigureGameTypeFlags` @ 0x00410CA0).
2. **Options edited** on RACE OPTIONS, SELECT TRACK (LAPS, DIRECTION), Quick Race (LAPS). These write
   `s_game_option_*` and `g_td5.ini.*` only.
3. **Launch**: `frontend_init_race_schedule` (`td5_fe_mp_setup.c`) re-commits the edited values into the runtime
   flags. Before round UB it did that for single race (game type 0) only, so every other mode raced the values of
   step 1, and some single-race options (collisions, checkpoint timers) were not re-committed at all.
4. **InitRace** (`td5_game.c`) applies the per-mode overrides (drag, traffic battle, net host config, the track's
   LEVELINF traffic flag) and logs `RACE OPTIONS USED`.

Time Trial is a single race at runtime (game type 7 is rewritten to 0 with `solo_mode_synth=1`: racer slots 1..5
inactive, traffic slots untouched), so traffic and police run normally in it.

## Matrix

Modes: **SR** single race (RACE > SINGLE RACE > SELECT TRACK), **QR** quick race, **TT** time trial, **Cup** SP cups
(track picker on), **Drag** SP drag, **Cop** SP cop chase, **MPR** local split-screen race / cup, **TB** traffic
battle, **MPC** MP cop chase, **MPD** MP drag, **Net** network race (host's values), **Roam** free-roam sprint /
circuit / free drive (own panels).

Cells: **R** offered and reaches the race. **FIXED** was lost or stale, reaches the race since round UB. **UE** decided
by Mariano and done in round UE (Q1-Q6 below). **VA** changed in round VA (below). **-** not offered. **own** the mode has its own control.

| Option | SR | QR | TT | Cup | Drag | Cop | MPR | TB | MPC | MPD | Net | Roam |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Traffic | R | R | **FIXED** | R per race | R (OFF/ON, ON = drag stream) | **FIXED** level, **UE** OFF = no traffic | R | **UE** hidden (forced VERY HIGH) | R | own | R | own |
| Police | R | R | **FIXED** | R per race | - | - | R (off when the split field > 6) | **UE** hidden (forced off) | - | - | R | own |
| Laps (circuits) | R | R | **VA** R (circuits; was hidden by UE) | R per race (Era 4) | n/a | **FIXED** | R | R | R | n/a | R | own |
| Difficulty | R | R | R (tier only) | - | **FIXED** | R | R | - | - | R | R | n/a |
| Opponents | R | R | - | - | R | R | R | - | R | R | host | own |
| Direction | R | R | R | R per race | n/a | R | R | R | R | n/a | R | forwards |
| Collisions | **FIXED** | **FIXED** | **FIXED** | **FIXED** | forced ON with traffic | **FIXED** | **FIXED** (gt 0) | forced ON | R | forced ON / R | R | n/a |
| Checkpoint timers | **FIXED** | **FIXED** | **UE** hidden (forced ON) | **UE** hidden (forced OFF) | - | - | - | - | - | - | off | - |
| Damage / deform | R | R | R | R | R | R | R | R | R | R | R | n/a |
| Catch-up | not shown, saved value applies | same | n/a | same | same | same | R | - | R | R | local | n/a |
| Lane assist | R | R | R | R | - | R | per player | per player | per player | - | per player | n/a |
| Auto gearbox | R | R | R | R | forced MANUAL (row greyed) | R | R | R | R | **UE** forced MANUAL (grey row on car select) | R | own |
| Speed units | R (global) | R | R | R | R | R | R | R | R | R | R | R |
| Weather | - (per track) | - | - | - | - | - | - | - | - | - | - | - |
| Time of day | - | - | - | - | - | - | - | - | - | - | - | own |

Geo city race (the GEOSPATIAL entry of SR / QR): traffic and police now come from the RACE OPTIONS rows (**FIXED**: it
read the runtime flags before the launch re-commit). Roam has 3 traffic levels: MEDIUM and above become HIGH.

## What round UB changed

| Change | Where | Test |
|---|---|---|
| TT: TRAFFIC, POLICE, COLLISIONS from RACE OPTIONS at launch | `td5_fe_mp_setup.c` `frontend_init_race_schedule` | `u5-traffic-timetrial` |
| SR / QR / MPR: COLLISIONS + CHECKPOINT TIMERS re-committed at launch | same | `u5-options-after-config` |
| Cop: TRAFFIC level + LAPS + COLLISIONS at launch (traffic stays ON) | same | `u5-copchase-traffic` |
| Cups: COLLISIONS at launch (game types 1..6 never wrote it) | same | (log line) |
| SP drag: DIFFICULTY tier at launch (the RACE OPTIONS pre-launch step never wrote it) | same | `u5-drag-difficulty` |
| Geo city race: TRAFFIC / POLICE from the rows | `td5_fe_race.c` `frontend_geo_entry_launch_roam` | - |
| `RACE OPTIONS USED` log line | `td5_game.c` InitRace | all u5 rows |

Each u5 row FAILS on a control exe with `td5_fe_mp_setup.c` reverted to 6610a463 and PASSES on round UB:

```
control  u5-traffic-timetrial   FAIL  time trial: traffic=0 vol=0 (asked 4) police=0 collisions=0, 0 traffic cars live at most
control  u5-options-after-config FAIL single race: collisions 1 (asked 0), checkpoint timers 1 (asked 0)
control  u5-copchase-traffic    FAIL  cop chase: traffic=1 vol=4 (asked LOW=1 after VERY HIGH), 16 traffic cars
control  u5-drag-difficulty     FAIL  drag: difficulty tier 0 (asked 2 after 0)
UB       all four               PASS  (time trial: traffic=1 vol=4 police=1 collisions=1)
```

## Round UE: Mariano's decisions (2026-11-08, "go with all recommendations")

| # | Decision | Where | Test |
|---|---|---|---|
| Q1 | Cop chase TRAFFIC OFF = an empty road (no longer forced HEAVY). LOW..VERY HIGH as picked. | `td5_frontend.c` ConfigureGameTypeFlags case 8, `td5_fe_mp_setup.c` frontend_init_race_schedule | `ue-q1-copchase-notraffic`: wanted=1 traffic=0 vol=0, 0 traffic cars in 20 s |
| Q2 | Traffic battle: TRAFFIC and POLICE rows hidden. | `td5_frontend.c` td5_raceopts_row_available | `ue-hidden-rows` |
| Q3 | Time trial: LAPS row hidden on SELECT TRACK (also on RANDOM). **Reverted in round VA**, see below. | `td5_fe_race.c` frontend_laps_row_mode_hidden (removed in VA) | `ue-hidden-rows` |
| Q4 | Time trial: CHECKPOINT TIMERS row hidden. | td5_raceopts_row_available | `ue-hidden-rows` |
| Q5 | Cups (SP; MP already hid it): CHECKPOINT TIMERS row hidden. | td5_raceopts_row_available | `ue-hidden-rows` |
| Q6 | MP drag: the car-select pane shows a grey, non-selectable MANUAL row between PAINT and OK. The per-player AUTO / MANUAL row lives on PROFILE SELECTION, which comes BEFORE the mode vote, so it cannot know drag yet; the car grid is the first screen after the vote. The player's own pick is not changed. | `td5_fe_mp_setup.c` mp_carsel_drag_gear_row | `ue-hidden-rows` + frames |
| - | Hidden rows leave no gap: QUICK RACE (CAR, TRACK, DIRECTION, LAPS) and SELECT TRACK (TRACK, DIRECTION, LAPS) stack their visible rows. RACE OPTIONS already built a filtered list. | `td5_fe_race.c` frontend_reflow_rows | `ue-hidden-rows` |

## Round VA (2026-11-09): LAPS back in time trial

Mariano: "there's no lap configuration on the latest version for circuits on time trials". Round UE's Q3 is reversed:

| Change | Where | Test |
|---|---|---|
| SELECT TRACK shows LAPS in time trial on circuit tracks, hides it point to point (same rule as a single race; RANDOM keeps the row) | `td5_fe_race.c` frontend_update_laps_button_visibility, frontend_random_track_rows (frontend_laps_row_mode_hidden removed) | `ue-hidden-rows` (expectations updated) |
| Time trial commits the LAPS row at launch, after ConfigureGameTypeFlags case 7 forced 1 | `td5_fe_mp_setup.c` frontend_init_race_schedule | `v2-tt-laps`: Maui, time trial, LAPS 1 -> 2 after the race type, natural finish, circuit_lap_count 2 and the player finished lap 2 |

CHECKPOINT TIMERS stays forced ON and hidden in time trial (Q4 unchanged).

## Questions for Mariano asked in round UB (answered above, kept for the record)

The original game forces each of these; the port's RACE OPTIONS screen still shows the row. For each: make it work, or
hide the row in that mode.

| # | Mode / option | Today | Recommendation |
|---|---|---|---|
| Q1 | Cop chase, TRAFFIC OFF | runs HEAVY (the original forces traffic on in cop chase) | **Make it work**: OFF = no traffic. Cop chase on an empty road is still a chase, and OFF on the row should mean OFF. |
| Q2 | Traffic battle, TRAFFIC + POLICE | TRAFFIC forced VERY HIGH, POLICE forced off (the mode is built on max traffic) | **Hide both rows** in traffic battle. |
| Q3 | Time trial, LAPS | 1 lap forced (original) | **Hide LAPS** in time trial: a time trial is one timed lap. (Alternative: honour it and time the best lap.) |
| Q4 | Time trial, CHECKPOINT TIMERS | forced ON (original) | **Hide the row** in time trial: the timers are the time trial. |
| Q5 | Cups, CHECKPOINT TIMERS | forced OFF at race init for every game type but 0 (original @ 0x0042ADAC) | **Hide the row** in cups. |
| Q6 | MP drag, AUTO gearbox | forced MANUAL (drag) while the per-player row offers AUTO | **Grey the row** in MP drag, as SP drag already does. |

## Also found

- Selftest rows that omit `.mp_mode` get 0, so the AutoRace MP override locked MP RACE and cleared the drag flags:
  `race-r8-drag` has not raced drag since that override existed (`gt=0 drag=0` on the drag strip). Fixed with
  `.mp_mode=-1` on the drag rows (now `gt=9 drag=1`, `InitRace: SP DRAG RACE`).
- POLICE is forced off in split screen with more than 6 cars on the grid (`td5_game.c` ~2531). Left as is: a limit of
  the police layer, not a lost option.
