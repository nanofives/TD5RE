# Traffic collision narrowphase (round 1026 M)

Mariano: "i need to refactor the logic of collisions with traffic, currently it detects as a crash when
nearby but not really touching".

## Result in one paragraph

Measured on 5 traffic races (Moscow AI + full-throttle, Sydney AI, Blue Ridge full-throttle, La Plata geo
full-throttle; 4000 sim ticks each, Traffic=4, 5 opponents): of 3318 racer-vs-traffic contacts that master
fired, **62.5 % had a real gap between the car bodies** (median 183 world units, p90 347, max 588; a car is
632 wide and 1450 long). Two independent defects, both in `td5_physics_collision.c`:

1. **Box rotation wired the wrong way round** in `obb_corner_test` (master lines 753/771/789/840). The
   original (`re/ghidra_export/functions/0x00408570_CollectVehicleCollisionContacts.c`, d = (yawA - yawB) >> 8)
   rotates B's corners into A's frame by R(+d) and A's corners into B's frame by R(-d); the port had the two
   pairs swapped. Identical for parallel cars (d = 0 or 180 deg, all the 501-sample pilot saw), wrong by 2d
   otherwise. Effect: false contacts up to 560 units apart AND real overlaps with no contact (365 measured,
   median depth 237 units: cars drawn through each other).
2. **The sphere paths reach far past the body.** `collision_detect_simple` (master 2168/2192: combined radius
   0.75*(rA+rB) of the cardef bounding spheres = 1223 for these two cars) is the whole contact test for
   (a) every airborne / scripted car (`wheel_contact_bitmask >= 0x0F` or `vehicle_mode != 0`, dispatch at
   master 2938) and (b) the S20 player-vs-traffic world-proximity supplement (master 3095..3106, gated on the
   FULL radius sum 1631). A pass in the next lane at 200-500 units clear fired an impulse. The same sphere
   also MISSES real nose-to-tail / head-on overlaps (7 of 98 probe poses).

Fix (default ON, each part has a knob): a traffic contact needs the two silhouettes to really overlap
(SAT over the convex hulls, int64, deterministic), and the box rotation matches the original.

## Knobs

| knob | default | effect |
|---|---|---|
| `TD5RE_TRAFFIC_SAT` | 1 | pairs with a traffic car: contact only on real hull overlap, in `collision_detect_full` and `collision_detect_simple` (sphere replaced by the SAT test, vertical bound = old sphere reach). 0 = master |
| `TD5RE_V2V_ROT_FIX` | 1 | corner-test rotation pairing as in the original. 1 = pairs with traffic, 2 = every pair (racer-vs-racer too), 0 = master |
| `TD5RE_V2V_GAPLOG` | 0 | dev: log the true gap of every fired contact (1 = traffic pairs, 2 = all) |
| `TD5RE_V2V_SELFPROBE` | 0 | dev: deterministic 98-pose detector probe at sim tick 90 |
| `TD5RE_V2V_STAGE` | off | dev: `start,end,lat,fwd,rel_deg` pin a traffic car beside slot 0 for a near-miss pass |

Knobs OFF (`TD5RE_TRAFFIC_SAT=0 TD5RE_V2V_ROT_FIX=0`) = master detectors (code path unchanged).

## Tools

* `verify/r1026m_gaprun.ps1` one traffic race, keeps race.log; `verify/r1026m_batch.ps1` the matrix;
  `verify/r1026m_gapstat.py` the per-path statistics. Lines: `v2v_gap: path=FULL|BP|S20|MISS ... gap= sat= ...`
  (gap = exact distance between the two silhouettes, 0 = overlapping; MISS = bodies overlap, corner test saw nothing).

## Measurements (5-run set; racer-vs-traffic rows)

```
### base
5756 contact lines from 5 log(s), traffic slot base 6
racer-traffic    ALL  contacts= 3318 gap>0= 2074 ( 62.5%) >50= 1823 >200=  983 med(gap>0)= 183.3 p90= 346.8 max=  587.8 | episodes= 422 with-gap= 284
racer-traffic    BP   contacts= 1316 gap>0=  418 ( 31.8%) >50=  300 >200=  168 med(gap>0)= 130.4 p90= 477.9 max=  587.8 | episodes=  79 with-gap=  45
racer-traffic    FULL contacts= 1926 gap>0= 1595 ( 82.8%) >50= 1465 >200=  768 med(gap>0)= 191.2 p90= 324.2 max=  561.9 | episodes= 342 with-gap= 231
racer-traffic    MISS contacts=  308 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  33 with-gap=   0
racer-traffic    S20  contacts=   76 gap>0=   61 ( 80.3%) >50=   58 >200=   47 med(gap>0)= 293.2 p90= 484.3 max=  545.3 | episodes=  27 with-gap=  21
### rot
3142 contact lines from 5 log(s), traffic slot base 6
racer-traffic    ALL  contacts= 1516 gap>0=  588 ( 38.8%) >50=  267 >200=  125 med(gap>0)=   7.6 p90= 330.4 max=  586.0 | episodes= 587 with-gap= 241
racer-traffic    BP   contacts=  582 gap>0=  208 ( 35.7%) >50=  189 >200=   76 med(gap>0)= 144.4 p90= 426.4 max=  544.9 | episodes=  50 with-gap=  24
racer-traffic    FULL contacts=  832 gap>0=  289 ( 34.7%) >50=    0 >200=    0 med(gap>0)=   1.0 p90=   1.9 max=    2.7 | episodes= 533 with-gap= 201
racer-traffic    S20  contacts=  102 gap>0=   91 ( 89.2%) >50=   78 >200=   49 med(gap>0)= 210.5 p90= 396.3 max=  586.0 | episodes=  26 with-gap=  22
### sat
2810 contact lines from 5 log(s), traffic slot base 6
racer-traffic    ALL  contacts= 1263 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes= 122 with-gap=   0
racer-traffic    BP   contacts= 1120 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  47 with-gap=   0
racer-traffic    FULL contacts=  113 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  82 with-gap=   0
racer-traffic    MISS contacts=   28 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  11 with-gap=   0
racer-traffic    S20  contacts=   30 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  13 with-gap=   0
### after
4929 contact lines from 5 log(s), traffic slot base 6
racer-traffic    ALL  contacts= 2026 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes= 328 with-gap=   0
racer-traffic    BP   contacts= 1325 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  65 with-gap=   0
racer-traffic    FULL contacts=  679 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes= 270 with-gap=   0
racer-traffic    MISS contacts=   43 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  43 with-gap=   0
racer-traffic    S20  contacts=   22 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  11 with-gap=   0
### final
5521 contact lines from 5 log(s), traffic slot base 6
racer-traffic    ALL  contacts= 2330 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes= 376 with-gap=   0
racer-traffic    BP   contacts= 1574 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  77 with-gap=   0
racer-traffic    FULL contacts=  741 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes= 318 with-gap=   0
racer-traffic    MISS contacts=   11 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  11 with-gap=   0
racer-traffic    S20  contacts=   15 gap>0=    0 (  0.0%) >50=    0 >200=    0 med(gap>0)=   0.0 p90=   0.0 max=    0.0 | episodes=  13 with-gap=   0
```

Reading: `base` = master detectors. `rot` = only the rotation fix: FULL contacts drop to gap <= 2.7 (rounding)
and the undetected overlaps vanish, BP/S20 sphere contacts still fire at a gap. `sat` = only the SAT gate:
no contact at a gap, but 179 undetected overlaps (median depth 44) remain. `after`/`final` = both.
MISS depth (overlap the detector did not see): base median 237 / max 454, final max 2.3 units.

7-run base set (adds Newcastle and La Plata AI, which hit the RaceTraceMaxSimTicks quit late): 5356
racer-vs-traffic contacts, 59.0 % with a gap, median 189, max 662.

## Racer-vs-racer: the same defect is real there (NOT changed by default)

Two Moscow/Sydney AI races, `TD5RE_V2V_GAPLOG=2`: master racer-vs-racer FULL contacts 498, **74.9 % with a real
gap** (median 71, p90 249, max 411), 121 undetected overlaps (median depth 32, max 399). With
`TD5RE_V2V_ROT_FIX=2`: 434 contacts, gap max 2.3, 4 undetected overlaps (max 1.9). Left at scope 1
(traffic pairs only) because the brief asked for racer-vs-racer unchanged; switching the default to 2
is a one-character change in `v2v_rot_fix_for` and is recommended.

## Deterministic probe (`TD5RE_V2V_SELFPROBE=1`)

98 poses of slot 0 vs a traffic car (side by side, angled 30/45/60, oncoming pass, head-on, rear-end,
tail-angled, T-bone), ground truth = hull SAT:

```
v2v_probe_sum: cases=98  master_full FP=7 FN=6 | rot_full FP=0 FN=0 | sphere FP=16 FN=7 | new_full FP=0 FN=0 | new_sphere FP=0 FN=0
```

FP = fires with the bodies apart, FN = misses a real overlap. Head-on and rear-end rams (lat 0, fwd 1000..1400
= overlapped by 65..465 units) fire in master_full, rot_full, new_full and new_sphere; they stop firing at
fwd 1500 (gap 35).

## Near-miss staged on the real track (`TD5RE_V2V_STAGE=480,560,900,3500,0`, Moscow, AI drives slot 0)

Master detectors: slot 0 passes a parked traffic sedan with the bodies **200 units clear** (S20 path), the
impulse kicks it sideways (vA.x -11943 -> -41626 in one tick) and sheds 12 % speed. New: no impulse
(vA.x stays +2000..3500, speed 551 -> 540, drag only). Frames in `log/r1026m_frames/`:
`stage_master_486.png` (contact tick, cars visibly apart), `stage_master_489.png` (kicked),
`stage_new_486.png`, `stage_new_489.png` (clean pass). The two runs are not tick-identical (AI start
variance), so compare the log lines, not pixel positions.

## Gates

* build_all.bat OK (dev + release), structure lint OK (extern 3/3, includers 23/23, warnings 84/84).
* selftest smoke 13/0/0, selftest full 64/0/0 (includes the traffic=4 races).
* synthetic identity gate (SELECTED.TXT aside): MODELS.DAT E2F1F33C221D61CAB0EFF701484011F0 (no generation touched).
* fork harness (`geo_fork_validate.ps1`, 3 seeds): Traffic=0 table identical to the same-tree master exe
  (F=89 PASS, F=790 PASS, identical rows); Traffic=4: master F=89 hit/crash 20/6 stall 7 spin 7, new 14/2
  stall 8 spin 3, both PASS (traffic runs are not deterministic: read as no regression, not as an improvement).

## Not verified / not fixed

* Vertical overlap (dy) is not part of the SAT (XZ only, as in the original); the simple path keeps the old
  sphere reach as a vertical bound.
* TD5RE_TRAFFIC_SAT does not touch the battle-mode swept wreck pass or the S17 anti-tunnel pass (the latter
  reads obb_corner_test, so it gets the rotation fix for traffic pairs).
* AI runs on Newcastle (5) and La Plata (61) never reached the 4000-tick quit with the knobs off
  (`base`) nor with them on (`after`): killed at the timeout, the game's own log stops mid-race. Cause not
  investigated (not compared against a stock td5re_master.exe run), probably the AI finishing the race so
  the RaceTraceMaxSimTicks quit gate is never reached.

## Round 1027 N: racer-vs-racer airborne contacts (`TD5RE_RACER_SAT`)

Mariano: "Fix the remaining airborne racer contacts". Round 1026 M gated only pairs with a traffic car, so
two racers on the sphere path (`collision_detect_simple`, dispatched for any car with
`wheel_contact_bitmask >= 0x0F` or `vehicle_mode != 0`) still fired at a real gap.

Fix: the same hull-SAT gate for racer pairs on the sphere path, plus a vertical bound. The bound is
`1.5 * (hyA + hyB)` where hy is each car's mesh max|y| (about 150 to 250 units, `mesh_height_store` from the
suspension envelope), capped by the old sphere reach and falling back to it when a height is unknown (traffic
pairs keep the old sphere reach). All int64, no floats, identical on every peer.

| knob | default | effect |
|---|---|---|
| `TD5RE_RACER_SAT` | 1 | 0 = master, 1 = sphere path needs real hull overlap + vertical bound, 2 = also reject FULL-path (corner test) racer contacts whose hulls are apart |
| `TD5RE_RACER_SAT_VERT_PCT` | 150 | percent of (hyA + hyB) used as the vertical reach |
| `TD5RE_V2V_PROBE_H` | 420 | dev: body height the airborne probe uses as ground truth |

Why default 1 and not 2: level 2 removes FULL contacts that fire with the hulls <= 2.6 units apart (box-vs-hull
rounding, not a real gap) but that changes every race from the first start-grid touch on.
`verify/shipped_trace.ps1 -Extra` A/B (RaceTrace, seed fixed, 3000 ticks, 5 AI, Traffic 0):

| run | Moscow (0) | Newcastle (5, jumps) |
|---|---|---|
| master vs `TD5RE_RACER_SAT=0` | IDENTICAL (108004 rows) | 4 rows differ, all the last tick (run-end truncation 33121 vs 33122 rows) |
| master vs default (1) | IDENTICAL (108004 rows) | differs from tick 210 (airborne racer contacts exist) |
| master vs 2 | differs from tick 161 | differs from tick 210 |

### Measurement (5 AI races, Traffic=4, 5 opponents, `TD5RE_V2V_GAPLOG=2`, 4000 sim ticks; Moscow 0, Sydney 2,
Newcastle 5 (jumps), Maui 6, Honolulu 8; Newcastle and Maui hit the 300 s timeout, the game log is still complete up to that point)

```
### base (td5re_master.exe = 7b221835)
racer-racer      BP   contacts=   21 gap>0=   11 ( 52.4%) >50=    6 >200=    1 med(gap>0)=  77.7 p90= 128.0 max=  337.5
racer-racer      FULL contacts= 1131 gap>0=   32 (  2.8%) >50=    0 >200=    0 med(gap>0)=   0.6 p90=   1.1 max=    2.2
racer-racer      MISS contacts=   20
racer-traffic    BP   contacts=  941 gap>0=    0
racer-traffic    FULL contacts=  435 gap>0=    0
racer-traffic    S20  contacts=  221 gap>0=    0
### default (TD5RE_RACER_SAT=1)
racer-racer      BP   contacts=   25 gap>0=    0 (  0.0%)                                                max=    0.0
racer-racer      FULL contacts=  839 gap>0=   20 (  2.4%) >50=    0 >200=    0 med(gap>0)=   0.6 p90=   1.5 max=    2.6
racer-racer      MISS contacts=   18
racer-traffic    BP   contacts=  545 gap>0=    0
racer-traffic    FULL contacts=  710 gap>0=    0
racer-traffic    S20  contacts=  108 gap>0=    0
### TD5RE_RACER_SAT=2
racer-racer      BP   contacts=   38 gap>0=    0
racer-racer      FULL contacts=  941 gap>0=    0 (  0.0%)
racer-racer      MISS contacts=   17
racer-traffic    BP   contacts=  505 gap>0=    0
racer-traffic    FULL contacts=  447 gap>0=    0
racer-traffic    S20  contacts=   69 gap>0=    0
```

Races are not tick-deterministic (AI start variance, see round 1026), so contact COUNTS differ between runs; read the
gap columns. Racer-racer BP per track at default: Moscow 6, Sydney 7, Newcastle 10, Maui 0, Honolulu 2, all with a
real overlap (gap 0). In the `TD5RE_RACER_SAT=2` Newcastle run the jump pile-up shows landings still register at dy up to 525
(ticks 1853..1872, slot 0 onto slot 3, SAT overlap 21 .. 737 units deep).

### Probe (`TD5RE_V2V_SELFPROBE=1`, `v2v_probe_r` rows, slot 0 vs slot 1)

98 planar poses: `new_sphere FP=0 FN=0` (master sphere FP=16 FN=8, same grid as the traffic probe).
128 airborne poses (8 landing layouts x dy 0..1500, both cars flagged airborne, truth = hulls overlap and dy < 420):
master sphere FP=34 FN=0, new sphere FP=5 FN=0 (the 5 are the deliberate 150 % margin: dy 500 against a 514
reach). The gap layouts (hulls 464 apart, 1100 lateral) fire 0 times at any dy; master fires at dy <= 400 for them.
The sphere keeps firing up to dy 1100 over a centred car in master, new stops at 514.

### Not fixed / not verified

* The vertical bound is the mesh max|y| of each car, not a real 3-D body test. If the origin is not at mid-height
  the bound is symmetric around it anyway; `TD5RE_RACER_SAT_VERT_PCT` scales it.
* Level 1 leaves racer-racer FULL contacts with a hull gap <= 2.6 units (the corner test uses the mesh box, the SAT
  the silhouette hull). Level 2 removes them.
* Battle swept-wreck pass is traffic-only and untouched; the S17 anti-tunnel pass gets the ROT_FIX=2 rotation only.
