# TD6 → TD5 car performance normalization (S07)

**Date:** 2026-06-04
**Feedback:** "adjust handling / acceleration / max speed of TD6 cars to TD5's range" —
out of the box every TD6 car felt categorically faster than the TD5 fleet.
**Touched (tracked):** `re/tools/convert_td6_cars.py` (rescale baked into the asset
pipeline), `re/tools/measure_car_perf.py` (measurement + audit).
**Regenerated (gitignored runtime data):** `re/assets/cars/<code>/carparam.dat` for
the 36 TD6 cars that ship a real param (donor-param cars aud/pro/xjr untouched).

## Why TD6 felt faster — the measurement

`carparam.dat` is byte-identical between TD5 and TD6 (drop-in), so the SAME
physics-tuning fields drive both fleets. The TD6 data simply sits above TD5's
range in the acceleration / handling / braking fields. Fields (file offset =
tuning base `0x8C` + tuning offset), consumed by `td5_physics.c`:

| field | tuning / file off | role | TD5 band | raw TD6 band |
|-------|-------------------|------|----------|--------------|
| grip      | 0x2C / 0xB8  | tire_grip_coeff (cornering / handling) | 2200–2750 | 2375–**5000** |
| torque    | 0x68 / 0xF4  | drive_torque_mult (acceleration) | 50–180 | 55–**240** |
| brake     | 0x6E / 0xFA  | brake_force | 400–750 | 450–**2800** |
| engbrake  | 0x70 / 0xFC  | engine_brake | 400–750 | 450–**2200** |
| speedlim  | 0x74 / 0x100 | speed_limit (top speed; torque cutoff) | 719–1159 | 635–1130 |
| redline   | 0x72 / 0xFE  | engine rev limit | 6200–7600 | 6200–6900 |
| drivetrain| 0x76 / 0x102 | RWD/FWD/AWD class (NOT a performance scalar) | — | — |

Key finding: **the dominant felt difference is acceleration (torque), not top
speed.** TD6's top-speed band (635–1130) is actually slightly *below* TD5's
(719–1159), but its torque reaches 240 vs TD5's max of 180, with the TD6 bulk
clustered at 148–240 while the TD5 bulk sits at 64–110. Grip and braking are
also far over range on the TD6 race/concept cars (grip up to 5000, brakes up to
2800). `redline` is already inside the TD5 band, so it is left faithful.

## The rescale

A per-field **linear min-max remap** maps each field's observed TD6 `[min,max]`
onto TD5's `[min,max]`:

```
new = TD5_min + (raw - TD6_min) * (TD5_max - TD5_min) / (TD6_max - TD6_min)
      clamped to [TD5_min, TD5_max]
```

This is monotonic, so it lands the slowest TD6 car near the TD5 slowest and the
fastest near the TD5 fastest, distributes the rest proportionally, and
**preserves the TD6 cars' ranking among themselves**. The TD6 source `[min,max]`
is computed over the full TD6 param set (`compute_td6_perf_ranges`) so a
`--codes` subset run uses the same global remap as `--all`. Implemented in
`convert_td6_cars.py` (`PERF_REMAP_FIELDS` + `remap_perf_fields`), applied per
car after the wheel adjustments; donor-param cars (aud/pro/xjr, whose carparam
is a TD5 car's) are skipped.

Fields remapped: grip, torque, brake, engbrake, speedlim. `redline` left
untouched (already within TD5's range). `drivetrain` (0x76) is never touched —
it is the RWD/FWD/AWD class, not a magnitude.

## Result

Converted TD6 fleet now spans **exactly** TD5's margins:

| axis | TD5 band | converted TD6 band |
|------|----------|--------------------|
| torque (accel) | 50–180 | 50 (att) – 180 (bmw/s12/toy/mcl) |
| top speed | 719–1159 | 719 (eli/tur) – 1159 (mcl) |
| grip (handling) | 2200–2750 | 2200 – 2750 (bmw) |
| brake | 400–750 | 400 – 750 |

The fastest TD6 car (mcl) lands on the TD5 ceiling (torque 180 = sp8, top speed
1159 = hot); the slowest (eli/tur by top speed, att by torque) lands on the TD5
floor. Relative ordering within TD6 is unchanged (monotonic remap).

Regenerate + audit:
```
python re/tools/convert_td6_cars.py --out-dir re/assets/cars
python re/tools/measure_car_perf.py            # raw TD5 vs TD6 ranges
python re/tools/measure_car_perf.py --converted# + post-rescale TD6 table
python re/tools/measure_car_perf.py --md       # before/after markdown (below)
```

## Before → after (per car, sorted by raw TD6 top speed)

TD5 reference band: torque 50..180, top-speed 719..1159, grip 2200..2750, brake 400..750

| code | torque (accel) | top speed | grip (handling) | brake |
|------|----------------|-----------|-----------------|-------|
| eli | 60->54 | 635->719 | 2375->2200 | 600->422 |
| tur | 60->54 | 635->719 | 2375->2200 | 600->422 |
| pwr | 62->55 | 636->720 | 2375->2200 | 600->422 |
| chr | 63->56 | 666->747 | 2375->2200 | 600->422 |
| cp2 | 70->61 | 666->747 | 2475->2221 | 600->422 |
| lit | 63->56 | 680->759 | 2375->2200 | 600->422 |
| mcj | 85->71 | 684->763 | 2375->2200 | 600->422 |
| flx | 63->56 | 705->781 | 2375->2200 | 600->422 |
| att | 55->50 | 710->786 | 2375->2200 | 600->422 |
| mam | 60->54 | 710->786 | 2375->2200 | 600->422 |
| chd | 78->66 | 723->797 | 2640->2256 | 450->400 |
| pan | 86->72 | 723->797 | 2475->2221 | 600->422 |
| sub | 81->68 | 729->803 | 2640->2256 | 450->400 |
| cp1 | 80->68 | 743->815 | 2600->2247 | 600->422 |
| esp | 88->73 | 782->850 | 2849->2299 | 2800->750 |
| grf | 94->77 | 809->874 | 2739->2276 | 2800->750 |
| atl | 93->77 | 832->894 | 2750->2279 | 2800->750 |
| shl | 94->77 | 836->898 | 2860->2302 | 2800->750 |
| cer | 157->122 | 855->915 | 2700->2268 | 2800->750 |
| cp4 | 149->116 | 855->915 | 2800->2289 | 2800->750 |
| sup | 99->81 | 856->915 | 2937->2318 | 2800->750 |
| lot | 94->77 | 870->928 | 2860->2302 | 2800->750 |
| xk1 | 135->106 | 880->937 | 2700->2268 | 2800->750 |
| 400 | 148->115 | 885->941 | 2800->2289 | 2800->750 |
| gts | 215->162 | 904->958 | 4200->2582 | 1200->512 |
| tus | 86->72 | 910->963 | 2475->2221 | 600->422 |
| db7 | 148->115 | 914->967 | 2650->2258 | 2800->750 |
| cp3 | 146->114 | 929->980 | 2750->2279 | 2800->750 |
| mgt | 149->116 | 929->980 | 2650->2258 | 2800->750 |
| lgt | 210->159 | 953->1002 | 4500->2645 | 1200->512 |
| 390 | 210->159 | 978->1024 | 4500->2645 | 1500->556 |
| bmw | 240->180 | 978->1024 | 5000->2750 | 2200->661 |
| s12 | 240->180 | 978->1024 | 2750->2279 | 2800->750 |
| g40 | 192->146 | 1061->1098 | 4000->2540 | 1200->512 |
| toy | 240->180 | 1100->1132 | 4000->2540 | 2200->661 |
| mcl | 240->180 | 1130->1159 | 4000->2540 | 2200->661 |

Donor-param cars (not in the table, intentionally unchanged): **aud, pro, xjr**
— they inherit the TD5 `tvr` carparam (torque 78, top 905, grip 2425, brake 700),
already inside the TD5 band.

## Notes / gotchas

- The difficulty multipliers (Normal torque ×0.5625, grip ×1.17; Hard etc., in
  `td5_physics.c` ~line 9942) apply equally to TD5 and TD6 cars, so normalizing
  the RAW carparam fields keeps the post-difficulty values aligned too.
- `config.nfo` spec-sheet numbers (TD6_STATS in the converter) are the
  *real-world* car specs shown on the SELECT-CAR stats screen and are NOT the
  physics inputs — they are intentionally left unchanged.
- Regen rewrites all per-car files, but only `carparam.dat` content changes;
  `himodel.dat` / `config.nfo` are byte-identical, and the skin/mask PNGs differ
  only in PNG container bytes (re-encode), not pixels.
