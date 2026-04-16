# Deterministic Validation Suite — 2026-04-09

Goal: give the whole RE project a repeatable validation set instead of subsystem-specific ad hoc checks.

Use this suite before and after high-risk RE changes.

## Tier 1: Core Drift Checks

### 1. RE artifact sync

Run:

```bash
python tools/check_re_sync.py
```

Purpose:

- catch naming/inventory drift before a coding or documentation pass starts

### 2. Standalone build

Run:

```bash
cd td5mod/src/td5re
build_standalone.bat
```

Purpose:

- fail fast on struct/layout drift and source integration regressions

## Tier 2: Deterministic Runtime Scenarios

### 3. Frontend cold boot

Purpose:

- validate intro skip, frontend initialization, screen dispatch, and basic input ownership

Watch:

- frontend screen index
- transition timing
- selected car/track defaults

### 4. Race standing start

Purpose:

- validate countdown, first live tick, gear selection, and initial camera ownership

Watch:

- timing fields
- player control bits
- engine speed
- camera transition state

### 5. Straight-line acceleration

Purpose:

- validate engine-speed accumulation, gear shifts, longitudinal acceleration, and drag

Watch:

- `engine_speed_accum`
- `longitudinal_speed`
- `current_gear`

### 6. Heavy braking into a corner

Purpose:

- validate steering clamp, slip, yaw response, and chase camera stability

### 7. Wall scrape

Purpose:

- validate track boundary normals, wall response, damping, and post-contact heading

### 8. Lap / checkpoint transition

Purpose:

- validate race progression ownership and end-state mutation

### 9. Save/load round-trip

Purpose:

- validate config persistence, cup progression persistence, and typed save structures

Watch:

- selected options
- unlocked state
- stored high-score fields

### 10. Asset decode sanity

Purpose:

- validate static.hed / texture page metadata / PRR structure assumptions against live assets

Watch:

- texture page counts
- mesh header sizes
- archive entry resolution

## Tier 3: Differential Mode

For race bugs, pair the suite with:

- `re/analysis/race-trace-harness.md`
- `re/analysis/race-validation-pack-2026-04-09.md`
- `tools/compare_race_trace.py`

Rule:

1. run one short scenario only
2. compare original vs port
3. stop at the first divergent stage
4. fix only that stage

## Recommended Always-Run Subset

If time is limited, run:

1. RE artifact sync
2. standalone build
3. frontend cold boot
4. race standing start
5. lap / checkpoint transition

That subset catches a large fraction of project-wide drift with low effort.
