# F2-D: roam selftest rows + spawn on road

Task F2-D of GEO FREE ROAM. Code: `td5_roam_mode.c` (spawn snap), `td5_roam_drive.c/.h` (scripted drive + verdict),
`td5_selftest.c` (rows `roam`, `roam-corners`), `td5_input.c` (one overlay line), `td5_track.c` (one fix, below).

## Spawn on road

`td5_roam_session_spawn` moves the spawn POINT onto the drivable surface before the car is placed.
The locator's push vector (click -> nearest boundary edge) plus a margin inward along the same direction
(1.5, 2.5, 3.5, 5, 7 m) is tried in order; the first candidate whose whole footprint (centre + 4 body corners at the
street yaw, 0.93 m half width, 2.2 m half length) is on the surface wins. A click that is on the surface but too
close to a kerb for the footprint gets a ring search (21 cm steps, 16 directions). The yaw (`spawn_yaw = -1`) is the
axis of the ribbon quad under the snapped point (near row centre to far row centre), or the first arm of the patch
node when the point is on a junction patch. The old code read the direction from the linked span origins and
returned yaw 0 on the lawn click (`ROAM_RESCUE` + 0.2 km/h creeping against a kerb).

Log (engine.log, before/after):
`roam: spawn snap to road: click place=(2631902,5429671) -> (2599744,5394361), moved 111.0 m` then
`roam: spawn slot=0 place=(2599744,5394361) world=(66100,-32311) yaw=1506 (street)`.
`td5_roam_spawn_snap_m()` returns the distance (0 = was on the road, -1 = no road found, the click is used as is).

## Selftest rows

Both rows launch a real free roam session at Plaza Moreno (-34.9214, -57.9545), slot 0 a human with
`auto_throttle` OFF, driven by `td5_roam_drive_bits` through the same overlay a keyboard uses (steering ramp,
physics, contact provider and wall code all run).

### `roam` (smoke + full)

Junction tour: street follower with a turn script (straight / left / straight / right, homing back to the
spawn past 1.4 km, preferring junctions not seen yet). At each junction the car follows a planned path from the
in-arm end row through the point where the two street centre lines meet to the out-arm end row (the corner-matrix
path), so it does not aim at an island. Ends after 24 DISTINCT junction clusters, brakes to rest, rests 3 s.

Verdict (FAIL on any):

| Invariant | Measure |
|---|---|
| never below the surface | chassis centre y under the locator ground at the chassis, every sim tick: 0 ticks. Wheel bottom under its ground by > 30 cm (F2-B measured -11 cm, this tour -19.5 cm) |
| never > 5 cm above it at rest | the provider's rest samples (< 0.5 km/h, wheel bottom - ground): max <= 5.0 cm, and at least one sample |
| never stuck | throttle held and < 5 km/h for more than 3 s in a row: 0 events. A wedge freed by the driver's reverse retry (throttle held and < 3 km/h for 1.5 s) is NOT a stuck event, it is counted and printed as `wedge N` |
| never off the surface | chassis centre inside a surface every tick; provider rescues (`ROAM_RESCUE`, `_Y`) = 0 INCLUDING the spawn |
| no pinch, no teleport | provider counters 0 |
| junction count | >= 24 distinct junction clusters crossed |

Typical row note: `roam 29 jn (24 distinct) 2.8 km, snap 111 m, rest -1.4 cm, below 0 stuck 0 resc 0 pinch 0 wedge 0`.
The standard degradation monitors (private bytes, handles, frame time) apply to the row as to every race row. The
invariant checker's "no progress" WARN is suppressed for roam rows (spans are -1 in roam, there is no span progress).

### `roam-corners` (full suite only)

The design 2.5 corner matrix, F2-B's picker and path in public-API form: teleport 45 m up the in-arm, pure pursuit
through a Bezier from the in-ribbon end row to the out-ribbon end row, 40 m out, speed held. Both speeds (40 and 80
km/h) run back to back at one corner. Per run: exit reached, 0 pinches, not stuck, 0 rescues.

Junction counts. Kinds as in F2-B (one-lane/service, calle x calle, avenida x calle, diagonal x avenida), junctions of
3-4 arms. Candidates in the whole casco:

| kind | candidates (<= 4 arms) | tested (spacing 120 m where plentiful) | candidates with up to 8 arms |
|---|---|---|---|
| one-lane/service | 11 | 11 | 13 |
| calle x calle | 825 | 20 | 955 |
| avenida x calle | 270 | 20 | 557 |
| diagonal x avenida | 4 | 4 | 9 |

Fewer than 20 of two kinds exist: the casco has 11 junctions where two one-lane or service ways meet at 90 +- 20
degrees (it is almost all two-lane calles) and 4 where a Diagonal meets an Avenida within 45-135 degrees with at most 4
arms (the diagonals cross at big multi-street junctions). A kind with no more candidates than the target is taken whole,
a plentiful kind is thinned to >= 120 m apart.

**F2-E (2026-10-17), verdict changed:** each corner now runs three passes, held 40 km/h, held 80 km/h and a
braking driver that arrives at 80 km/h and slows for the bend (bend speed sqrt(6.0 m/s^2 x R), braking 7.0 m/s^2,
look-ahead 0.25 x km/h metres within 6-16 m). Pinches and rescues fail on every pass; exit and stuck are judged on
the braking pass only (a held-speed 135 degree city corner is not a fair stuck test). A Bezier that leaves the road
is routed on the paved mask first (00064c21000864e3: 16.4 m off, 18-point route). Note on the default set:
`ok corners: 165 runs (11/20/20/4), brake exit 55/55 stuck 0, held exit 110/110 stuck 0, pinch 0, resc 0`;
with 8 arms: brake 62/62, held 123/124 (00019ab000083a0f held 80 stops on the median nose, reported), pinch 0.
`TD5RE_ROAM_SELFTEST_CORNER_BRAKE=0` restores the two held passes, all judged; `_CORNER_ROUTE=0` keeps the Bezier.

**F2-E: the KNOWN exemption is removed; nothing is excused.** On the rules-v4 tiles the
cluster below exits at 80 km/h (2 reverse retries, 0 pinches) and the default set is 110 / 110 exits, 0 pinches,
0 rescues. The 8-arm pinches below are 0 with the F2-E wall probes (edge midpoints, tip rule); 00019ab000083a0f
at 80 km/h now stops on the median nose instead of sliding through it (the 16 m look-ahead cuts the inside of the turn).

Was KNOWN: cluster `00064c21000864e3` (diagonal x avenida, 134 degrees) at 80 km/h wedges (stuck after 2 retries, 250 wall hits,
1 pinch-free stall). That is task F2-E's acute-junction wedge. The row reports it as `KNOWN(F2-E) 1` and does not
fail on it; the same cluster at 40 km/h passes. A KNOWN run that passes is counted as a pass.

With up to 8 arms (`TD5RE_ROAM_SELFTEST_CORNER_MAX_ARMS=8`, off by default) two more runs pinch: cluster
`00019ab000083a0f` (avenida x calle, 90 degrees, 80 km/h: 203 pinch ticks) and `00033158000788a8` (diagonal x avenida,
128 degrees, 80 km/h: 125 pinch ticks). Neither is in the default set, both are reported to the orchestrator.

Report file: `log/roam_corner_test.txt` (one line per run + per-kind summary).

## Standalone use

`TD5RE_ROAM_SELFTEST_DRIVE=route|corners td5re.exe --Roam=1 --RoamSpawn=-34.9214,-57.9545 --TraceFastForward=8`
runs the same script without the director, logs `[ROAMSELFTEST]` lines and a verdict, and quits.
Knobs: `TD5RE_ROAM_SELFTEST_JUNCTIONS` (24), `_KMH` (40), `_MAX_SECS` (420), `_CORNERS_PER_KIND` (20),
`_CORNER_GAP_M` (120), `_CORNER_MAX_ARMS` (4).

## A bug the selftest exposed (fixed)

A free roam race after any other race in one session crashed in `td5_hud_init_minimap_layout`: a level loaded
without a strip (`td5_track_load_strip(NULL, 0)`, which free roam relies on) returned the placeholder display lists
without clearing `g_strip_span_count / _total_segments / _span_base / _vertex_base`, so the HUD minimap init walked
the PREVIOUS race's freed span array. The dev CLI never saw it (no previous race). `td5_track.c` now clears the four
globals on that path.
