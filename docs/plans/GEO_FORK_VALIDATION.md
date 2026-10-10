# GEO FORK VALIDATION (round 1016 H)

An automatic, repeatable judge for the driveable forks of a geo route: does the AI get
through each fork cleanly, compared with the same route with the forks switched off?
Built so a relaxed fork (looser bend / lane-change / left-corridor vetoes, variable-length
plaza corridors) is KEPT only when the field passes it, instead of by looking at it.

```
pwsh verify/geo_fork_validate.ps1                       # 3 seeds, forks on + forks off (6 races in parallel)
pwsh verify/geo_fork_validate.ps1 -Only 1130            # build ONLY the fork whose F (main span) is 1130
pwsh verify/geo_fork_validate.ps1 -Only 94,210          # a candidate list
pwsh verify/geo_fork_validate.ps1 -Deny 328             # everything except F=328
pwsh verify/geo_fork_validate.ps1 -ForkEnv "TD5RE_GEO_PLAZA_STRETCH_MAX=3.0"   # a candidate build (fork arm only)
pwsh verify/geo_fork_validate.ps1 -BaselineFrom verify/out/<tag>   # reuse that run-set's forks-off races
pwsh verify/geo_fork_validate.ps1 -Analyze verify/out/<tag>        # re-judge saved runs, no races
python verify/geo_fork_validate.py verify/out/<tag> --out x.json   # the analyzer alone
```

Needs a dev `td5re.exe` at the worktree root (built with the knobs below) and the geo
place (`re/assets/geo/la_plata_partido`, found in the worktree or, for a worktree, the main
tree through `git rev-parse --git-common-dir`). Exit status 0 = no FAIL, 1 = some fork FAILs.
Everything it writes lands in `verify/out/` (gitignored).

## What a run is

`verify/out/<tag>/{fork,base}_s<seed>/` per race:

| arm | what is different |
|---|---|
| `fork` | the real forks built (all of them, `-Only`/`-Deny` subset, or a candidate build via `-ForkEnv`), every AI fork choice pinned so each fork has cars on BOTH arms |
| `base` | the SAME seed with `TD5RE_GEO_REAL_FORKS=0` (the round-1012 route: no corridors, no widened windows): the built-in A/B |

Both are 6 AI cars (`PlayerIsAI=1`, `--DefaultOpponents=5`), no traffic, car damage OFF
(damage ON freezes racers like a stall), min graphics, RT off, `--Windowed=1 --VSync=0`,
window to the back, audio off, no network, td5re.ini never written (a run dir has no ini; the
CLI carries every setting), every inherited `TD5RE_*` cleared before each launch.

**Runs do not clobber each other.** Each race gets its own directory with a COPY of the exe
(the game writes `log/` next to the exe and every asset path is relative to the working
directory), a real `re/assets/` whose static children are NTFS junctions into the shared tree,
a private `levels/level091` (the generator deletes and rewrites it) and a private geo place
(hard links for the 4 big rasters, a copy of `_route/`; `_tiles/`, the network tile cache,
is left out). Titles are distinct (`TD5RE forkval <tag> <run>`); every kill is by the PID the
script started (graceful `CloseMainWindow` first, so `race.log` flushes). Teardown removes the
junctions with a reparse-aware delete that never descends into one.

### Seeds

The AI field is fully deterministic (per-slot skill from the difficulty tier, no
traffic); the race seed only fed the fork-choice roll, which the harness pins. So a "seed"
here is three things that DO change the race: the **fork-choice phase** (which half of the
field takes the corridor, hashed from the seed and the fork), the **AI tier** (`-Difficulty
auto` = 1, 2, 0, 1, 2, 0 by seed index: normal, hard, easy), and the race seed itself.
Three different seeds are three different races; the same seed is bit-for-bit the same race.

### Knobs added (dev builds only, compiled out of RELEASE)

| knob | where | meaning |
|---|---|---|
| `TD5RE_AI_BRANCH_FORCE_MODE` | `td5_ai.c`, next to `..._FORCE_P0` | 1 = alternate by slot parity, phase = hash(race seed, fork span): half the field each way at EVERY fork; 2 = everyone takes the corridor; 3 = everyone stays on the main road. Unset = the roll. Same RNG step either way. |
| `TD5RE_GEO_FORK_ALLOW` / `_DENY` | `td5_dev_forkfilter.h` (header-only), one call in `tg_realfork_build` | comma list of fork F (main span) values. DENY drops those, ALLOW builds ONLY those, `-1` builds none. Applied to the RESULT of the weighted selection, so the surviving forks keep exactly the windows and tapers they have with everything on. |

## What is measured

Per fork, summed over the seeds, in BOTH arms over the SAME main-span window. The window is
`F-8 .. R+2` (the engine's approach and tail) plus the corridor; where two forks' windows
overlap (Diagonal 73 `R=209`, plaza `F=210`) the shared spans go to the earlier fork up to
the midpoint. A **car-pass** is one car entering the window.

| column | meaning |
|---|---|
| `enter c/m` | car-passes that drove the corridor / stayed on the main road |
| `wall in/body/out` | contact events (0 -> non-zero `track_contact`) at the entry mouth (`F-8..F+4`, corridor step <= 4), the body, the exit mouth (`R-4..R+2`, last steps). Informational, not graded: a scrape that costs nothing is not a problem |
| `hit/crash` | wall INCIDENTS (one car's contacts <= 90 ticks apart) graded by the speed they cost over the next 25 ticks: **hit** = lost >= 40 % of the entry speed, **crash** = left <= 15 % of it (or reversed). Slower than 120 u/t at entry = a shunt, ignored |
| `stall` | a car in the window making <= 2 spans of progress for >= 90 ticks (3 s) |
| `jam` | a stall that lasted >= 300 ticks, or was still going when the trace ended: the pile-up signature |
| `spin` | for >= 4 consecutive ticks the nose points back along the road (> 110 deg off the span heading) or the car is side-on (> 60 deg off its own velocity at > 120 u/t). Nose = `(euler_yaw >> 8) & 0xFFF` of the rotation trace, same circle as `atan2(vx, vz)` of the pose |
| `offrd` | >= 4 ticks beyond 1.2 half-widths of the span it is on (lateral from the pose against the strip rows in `STRIP.DAT`) |
| `speed f/b/main` | mean forward speed (u/t = 24.8 speed / 256) in the window with forks / the same window forks-off / the plain main road of the fork run |
| `BASE ...` | hit / crash / stall / spin of the same window in the forks-off race |

Cars are only counted from their first movement to the moment they reach the finish area
(ring end minus 100 spans); rows after that are a parked car.

## Verdict (thresholds, all in `THRESHOLDS` at the top of `geo_fork_validate.py`)

Everything is **fork minus baseline, per car-pass**, so a window that is already ugly without the
fork (the Azcuenaga ring road) is not blamed for what it already had. FAIL on any FAIL line,
else WARN on any WARN line, else PASS. A fork with fewer than 3 corridor entries is
**UNTESTED** (nothing learned; neither allowed nor denied).

| rule | WARN | FAIL |
|---|---|---|
| jammed / trapped cars | >= 1 extra | >= 0.10 per car-pass |
| stalls | >= 1 extra | >= 0.25 per car-pass |
| window mean speed vs forks-off | < 80 % | < 60 % |
| crashes | >= 0.10 per car-pass | >= 0.25 |
| hard hits (incl. crashes) | >= 0.25 | >= 0.50 |
| spins | >= 0.15 | >= 0.34 |
| off-road episodes | >= 0.10 | >= 0.34 |

Why these numbers: jams and stalls are what a pile-up is, so a single extra jam is already a
WARN and 1 jam per 10 car-passes a FAIL. Harm rates are anchored on the baseline noise of
the master forks (3 seeds = 18 car-passes per fork, so one event moves a rate by 0.055: a
single crash is below every WARN line, three crashes is a WARN, five a FAIL). **Use 3 seeds
or more to decide; one seed (6 car-passes) is a screening run** (one event = 0.17).

## The machine-readable verdict: `verify/out/fork_validate.json`

```json
{
 "schema": 1,
 "generated": "2026-10-10T11:04:35",
 "run_set": "master_c", "place": "la_plata_partido", "track": 61,
 "exe": "td5re.exe", "seeds": [11, 22, 33], "fast_forward": 8.0,
 "fork_env": "", "only": "",
 "thresholds": { ... },
 "overall": "FAIL",
 "allow":    [901],            // PASS
 "warn":     [46, 210, 1130],  // WARN: caller's choice (--warn-allows folds them into allow)
 "deny":     [328],            // FAIL
 "untested": [],               // < 3 corridor entries: no verdict
 "env": { "TD5RE_GEO_FORK_ALLOW": "901", "TD5RE_GEO_FORK_DENY": "328" },
 "forks": [ { "key": "F328", "F": 328, "R": 565, "len": 236, "kind": "avenue",
              "name": "Diagonal 73", "corridor": [1591, 1826],
              "verdict": "FAIL", "reasons": ["FAIL: hard hits ... 9 vs baseline 0 (+0.50 per car-pass)"],
              "fork": { "passes": 18, "took": 9, "hits": 9, "crashes": 3, "stalls": 1, ... },
              "base": { ... same keys, forks off ... },
              "speed": { "speed": 585.7, "base_speed": 651.3, "ratio": 0.899, "main_speed": 358.4 },
              "stalls": [[slot, tick0, tick1, span_lo, span_hi]], "spins": [[slot, tick, span_raw]] } ] }
```

The key is **F, the fork's main span** (stable: the corridor spans move when another fork is
dropped, F does not). A generator knob consuming it needs only `allow`/`deny` (or the
`env` block verbatim): `TD5RE_GEO_FORK_ALLOW` / `TD5RE_GEO_FORK_DENY` already exist as dev
knobs and read exactly these lists. **Not wired into the generator as a shipping feature**:
the dev filter is the only consumer, compiled out of RELEASE. If a shipping allow/deny list
is wanted later, read this file at the same place (`tg_realfork_build`, after
`rf_select`) behind a knob; `F` values are only meaningful for the route they were measured
on (a different route or a changed selector gives different `F`), so key the file by
`place` + `track` and refuse it when the fork table differs.

## Speed: how fast can a race run, and is it the same race?

`--TraceFastForward=N` scales the frame delta that feeds the fixed 30 Hz sim accumulator, so
the sim runs the SAME ticks, just sooner. Proof, same seed (11), fork arm and baseline, run at
1x / 4x / 8x / 16x / 32x (`verify/geo_fork_validate_equiv.py`):

* **Per-fork table: identical** at every speed (every count: entries, hits, crashes, stalls,
  jams, spins, off-road; only the mean speed moves in the last digit).
* **Per-tick positions: bit-identical** (world x, z, yaw of all 6 cars every tick) from tick 0
  until the first car finishes: fork arm 3875 of 4076 ticks, baseline 3536 of 3737 (4x:
  all 3737). After the first finish the cars park and the end-of-race handling is
  frame-dependent (<= 8.5 world units), which is after the last fork's window.

Default 8x. Wall time of one 3-seed batch (6 races in parallel, generation 2 s each): **88-107 s**
end to end (vs 3 x ~12 min serial); 1x takes 222 s for the SAME 2 races (measured with 8 games running at once), so 8x is the knee (16x
103 s, 32x 97 s: the floor is process start, level build and the 8 s race-over wait, not the
sim). A single `-Only` fork with a reused baseline: **46-61 s**.

A race stops when (first that applies) every car is past `min(last fork rejoin + 40, finish -
5)` or has not advanced for 900 SIM ticks (a jammed car must not hold the other five hostage);
or the sim stops ticking for 8 wall seconds (the race ended); or no new span record for 45 s;
or the wall cap.

## Master as is: the 5 forks (La Plata Partido, cdce612c + this harness, 3 seeds, 8x)

`verify/out/master_c` (88 s wall). u/t speeds; `b` = forks off.

| fork | F..R | corridor | enter c/m | hit/crash | stall | jam | spin | speed f/b | BASE hit/crash/stall/spin | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| Diagonal 73 (avenue) | 46..209 | 1311..1472 | 9/9 | 8/3 | 0 | 0 | 2 | 471/494 | 6/0/1/2 | WARN crashes 3 vs 0 |
| Plaza Miguel de Azcuenaga | 210..327 | 1474..1589 | 9/9 | 23/9 | 6 | 1 | 17 | 342/369 | 27/7/8/12 | WARN 1 jam, spins 17 vs 12, crashes 9 vs 7 |
| Diagonal 73 (avenue) | 328..565 | 1591..1826 | 9/9 | 9/3 | 1 | 0 | 1 | 586/651 | 0/0/0/0 | **FAIL** hard hits 9 vs 0 (+0.50) |
| Avenida 60 | 901..1075 | 1828..2000 | 8/10 | 11/0 | 0 | 0 | 1 | 521/537 | 7/0/0/12 | PASS |
| Avenida 7 | 1130..1174 | 2002..2044 | 7/8 | 4/0 | 0 | 0 | 0 | 392/441 | 0/0/0/0 | WARN hits 4 vs 0 |

Reading it: every fork got cars on both arms (7..9 corridor entries each, forced). No
fork jams or piles up the field; the harm is wall incidents at the mouths. The one that
matters is **F=328 (Diagonal 73, 236 spans)**: forks off the window has 0 incidents, forks
on it has 19, 3 of them crashes at the REJOIN (span 565..566, cars at 800+ u/t arrive on a
3-lane merge and lose everything). Avenida 60 is clean. The Azcuenaga plaza is a bad stretch
with or without its corridor (baseline: 34 incidents, 12 spins, 8 stalls).
Run in isolation (`-Only 1130`) Avenida 7 is PASS (3 hits, 0 crashes over 8/8 entries): its
WARN in the full run comes from cars arriving upstream already in trouble.

## Known-bad case: Plaza Dardo Rocha with the fixed-length corridor

`-ForkEnv "TD5RE_GEO_PLAZA_STRETCH_MAX=3.0"` builds the plaza the 1015 E selector refused
(stretch 2.21x against the 1.6 cap, free chain 478 m over 62 spans). `verify/out/dardo_final`
(all forks, 73 s wall) and `verify/out/dardo_only` (`-Only 1031`, 46 s wall):

| | F..R | corridor | enter c/m | hit/crash | stall | jam | speed f/b | BASE hit/crash/stall | verdict |
|---|---|---|---|---|---|---|---|---|---|
| Dardo Rocha, all forks on | 1031..1129 | 1957..2053 | 7/10 | 21/10 | **43** | **14** | 163/386 (**42 %**) | 17/5/6 | **FAIL** |
| Dardo Rocha alone | 1031..1129 | 1311..1407 | 9/9 | 29/16 | **43** | **8** | 160/393 (**41 %**) | 17/5/6 | **FAIL** |

FAIL reasons (alone): 8 jammed cars (+0.44 per car-pass), 43 stalls vs 6 (+2.04), 16 crashes
vs 5 (+0.59), 29 hard hits vs 17, window speed 41 % of forks-off. That is the 1015 E pile-up,
caught by four independent rules, at 3x the FAIL line on the jam rule. Side effect worth
knowing: with Dardo forced in, nobody took Avenida 7's corridor (0 entries, 10 stayed on the main
road; the cause was not investigated, a jam upstream is the obvious suspect): reported
UNTESTED, not PASS.

## Recommended use

* **Decide on 3+ seeds** (`-Seeds 3` is the default). One seed is a screen.
* A candidate fork: `-Only <F> -ForkEnv "<its relaxing knobs>" -BaselineFrom verify/out/<master run-set>`
  (46-61 s). Then the whole set with all candidates once, because forks interact (Avenida 7).
* Keep PASS; investigate WARN (the `reasons` say what); drop FAIL and UNTESTED.
* Re-run the master set after any change to AI steering, fork mouths or the selector: a
  fork that was PASS can move. The harness is deterministic, so two runs of the same exe give
  the same counts (verified: three runs of the master set, `master_a/b/c`, differ only in a baseline mean speed by 1 u/t, because the stop moment is wall-clock).
* Thresholds are relative to the baseline per car-pass; if the baseline route changes (a
  different place, buildings that change the main road) re-run a baseline, do not reuse one.

## Limits

* AI only. Nothing here says a human can drive the fork, or what traffic does there.
* Fork choice is pinned (half each way). It measures "can the field pass", not how often
  the random AI picks it.
* The baseline is the SAME window without the corridor, so the plaza's ring road shows
  its own problems in both arms; only the difference is graded.
* Speed is `long_speed / 256` (u/t); 24.8 world units per sim tick. Comparisons are
  relative.
* One `race.log` per race carries the fork table (`trackgen: fork ... corridor=a..b
  rejoin=R (ring=N)`); an exe that changes that line needs the regex in the analyzer.

## Round 1017 R additions

* The fork arm sets `TD5RE_GEO_FORK_VERDICTS=0` (the committed verdict table must not hide the forks the harness measures).
* `fork_validate.json` is schema 2: `route_fp` and `route_place` (the `[REAL FORK] route fingerprint` the generator logs) identify the exact route.
* `verify/geo_fork_verdicts_gen.py a.json b.json ...` turns run-sets into `td5_geo_fork_verdicts.h`; `verify/geo_fork_keep.json` holds the reason for every WARN fork kept.
* Decide with 6 seeds (`-Seeds "11,22,33,44,55,66"`, ~140 s with a reused baseline ~80 s): a single jam or stall in 36 passes is below every FAIL line but is a WARN.
