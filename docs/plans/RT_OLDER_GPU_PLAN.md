# RT lighting on older GPUs (RTX 3070 class): audit, fixes, plan

Round UA, 2026-11-08. Mariano's report: "on my 3070 it struggles to load and the FPS is not stable".
Measured on the dev machine (RTX 5070 Ti 16 GB, Windows 11), Moscow unless stated, dev build,
`verify/rt_bench.py` (AutoRace + full throttle, 1800 frames after 120 skipped, windowed, VSync off).
The window cannot exceed the desktop here: "1440p" runs are **2542x1333** (close to Mariano's 2560x1351);
4K runs were clamped to the same size and are not used.

## How to measure (new tooling)

| Tool | What it gives |
|---|---|
| `TD5RE_FRAMESTATS=N` (td5_profile.c) | wall-clock frame-to-frame time of N race frames: mean / p50 / p90 / p95 / p99 / p99.9 / max, spikes (> 2x p50), > 33 / 50 / 100 ms counts, the 8 worst frames, process start -> first race frame (load). Appends to `log/framestats.txt`, then quits cleanly. |
| `TD5RE_RT_TIMESTAMPS=2` (d3d12_backend.c) | GPU timestamps at every RT pass boundary (`rt.<pass>.trace / .denoise / .comp`, `rt.blas`, `rt.tlas`), summed per frame, percentiles in the same report. Each trace/denoise stamp follows a UAV barrier, so those numbers are real; raster draws are not barrier-fenced, so the `pre.*` / `*total` columns under-count raster work and are not used below. |
| VRAM | `IDXGIAdapter3::QueryVideoMemoryInfo` (LOCAL) every 30 frames: usage, peak, OS budget. |
| PSO / state object | count + total + max compile ms (graphics PSOs and the DXR state object), mid-frame count. |
| `python verify/rt_bench.py --tag X --preset LOW/MEDIUM/HIGH/ULTRA [--res WxH] [--set Key=V] [--env K=V] [--row selftest-row]` | one run, prints the block. `--env TD5RE_FRAME_CAP=0` removes the 144 fps cap (use it to see GPU-bound frame time). `--row roam-traffic-smoke --env TD5RE_SELFTEST_RT=1` benchmarks a scripted roam drive with RT forced on. |

## Inventory: every lighting option, what it costs, whether it worked

GPU cost: LIGHTING HIGH, 2542x1333, p50 ms per frame (before round UA's half-res change).

| Option (LIGHTING OPTIONS row / tier) | GPU work | Cost | Worked before round UA? | Now |
|---|---|---|---|---|
| LIGHTING QUALITY LOW/HIGH | HIGH = per-frame TLAS + windowed BLAS feed + the 4 passes below, each = DispatchRays at full res + 3 a-trous denoise iterations (full-res compute) + a full-screen composite | TLAS 0.03, BLAS streaming ~0 (p99 0.13, max 1.25) | yes, live | unchanged |
| SHADOW QUALITY LOW/MED/HIGH = 1/4/8 sun rays (cars at least 6) | sun shadow trace | trace 0.51, denoise 0.78 | **restart only** (latched on first use) | live |
| SHADOW DETAIL HALF/FULL | resolution of shadow + GI + dynamic-light trace | n/a | **dead**: never wired, always FULL | HALF = 1 ray-gen thread per 2x2 block + depth/normal-aware upsample |
| REFLECTIONS OFF/HALF/FULL | reflection trace (1 ray on reflective pixels) | trace 0.33, denoise 0.60 | HALF == FULL (dead); **OFF cleared the shared SSR toggle and never restored it** (OFF -> FULL left every reflection off, LOW too) | HALF = half-res; OFF only gates the RT pass |
| REFLECTION RANGE NEAR/FAR/UNLIMITED | ray TMax 4000 / 50000 / 1e7 | small | restart only | live |
| GLOBAL ILLUM. OFF/LOW/HIGH = 2/4 rays | sky-visibility AO trace | trace 0.37, denoise 0.88 | restart only | live |
| SUN & SKY CLASSIC/AUTO | sun disc + (should be) image-probe sun | ~0 | only the disc; the probe sun stayed on | CLASSIC = zone sun, no disc; live |
| LIGHTS BASIC/REALISTIC = 1/2 shadow rays per light + soft cone | per-pixel loop over up to 32 lights | trace 1.05, denoise 0.94 | restart only | live |
| CAR LIGHTS, LEGACY SHADOWS | raster | n/a | yes, live | unchanged |
| PERFORMANCE > LIGHTING LOW/MEDIUM/HIGH/ULTRA | presets of the rows above (main.c `k_light_tier`) | see below | MEDIUM's "half-res" was dead, so MEDIUM traced full-res | MEDIUM is real half-res |

The denoiser costs more than the ray tracing: 3.2 ms of the 6.0 ms (HIGH, 2542x1333, p50). Four masks x 3 a-trous
iterations x 25 taps x (mask + depth + G-buffer) at full res is bandwidth, which is exactly where a 3070
(448 GB/s) is weakest against a 5070 Ti (896 GB/s).

## Tier cost (GPU, ms per frame, p50 / p99, RT passes only)

| Tier | 1920x1080 before | 1920x1080 after | 2542x1333 before | 2542x1333 after |
|---|---|---|---|---|
| MEDIUM | 1.97 / 4.34 | **0.63 / 0.92** | 3.29 / 5.77 | **0.96 / 1.31** |
| HIGH | 3.31 / 5.17 | 3.40 / 4.69 (unchanged config) | 6.04 / 9.18 | 5.41 / 7.49 (same config, run noise) |
| ULTRA | 4.02 / 5.59 | 4.19 / 5.68 | 6.51 / 8.85 | 6.63 / 9.03 |

Means: MEDIUM 2.16 -> 0.65 ms (1080p) and 3.44 -> 0.97 ms (2542x1333), i.e. -70% / -72%. Two repeats of the
"after" matrix agree within 0.05 ms on MEDIUM; HIGH / ULTRA p99 moved up to 2 ms between repeats (other
sessions share this GPU).

Uncapped whole-frame time after the change (CPU+GPU, p50 / p99): LOW 1.47 / 2.64 and 1.56 / 4.75;
MEDIUM 1.72 / 3.36 and 1.87 / 4.93; HIGH 3.77 / 6.44 and 5.60 / 8.56 (1080p and 2542x1333).
3-way split screen 1921x1081 HIGH: 3.96 ms full-res vs 1.40 ms half-res.

**3070 projection (an estimate, not a measurement: no 3070 here).** Assuming a 3070 traces 2.5-3x slower than
the 5070 Ti, HIGH at 2560x1351 costs ~13-16 ms of RT alone with p99 ~18-22 ms: on top of raster that is the
45-60 fps wobble Mariano saw. Old MEDIUM was ~8-10 ms (p99 ~14-17). New MEDIUM is ~2.4-2.9 ms (p99 ~3.3-3.9),
which leaves a 60 fps frame (16.7 ms) most of its budget even at 1440p.

## Load ("struggles to load")

| Step | Cost here | Notes |
|---|---|---|
| DXR state object compile (`CreateStateObject`, one DXIL library) | 177-235 ms with a WARM driver shader cache | cold cache (first launch after a driver update) not measured: wiping the driver cache would hit other apps |
| BLAS warmup on the loading screen | 40-56 ms (Moscow, auto track) | |
| graphics PSOs | 15 compiles, 5 ms total | no pipeline library needed |
| race load, process start -> first race frame | LOW 2.63-2.75 s, HIGH 2.87-3.21 s | RT adds ~0.2-0.5 s here |

Fix: the state object now compiles on a worker thread from the first frontend frame (and at the start of
InitRace for AutoRace), joined at first RT use. From the menus the race load no longer waits for it.
The RT warmup itself is small. VRAM is not the problem: HIGH peaks at 665-695 MB local (LOW 200-280 MB) against
a 15 GB budget here, so ~0.5 GB of RT on an 8 GB card.

## Free roam

The RT world feed walks the MODELS.DAT display list. The free-roam city has none (its geometry is roam tiles),
so in roam the TLAS held only the cars, while shadow / GI / reflection still ran (1.8 ms at 2542x1333) and
replaced the screen-space sun shadows with an empty-scene result. Framedumps LOW vs HIGH at the same tick are
nearly identical. Round UA: free roam runs the LOW lighting stack (`TD5RE_RT_ROAM=1` restores the old path).

## Other defects found

- DXR tier: the pipeline is `lib_6_5` and calls `GeometryIndex()` (DXR 1.1) but the backend and the
  auto-detect accepted tier 1.0, so a 1.0-only device (GTX 10xx/16xx DXR fallback) would offer RT and then fail
  `CreateStateObject` while the game switched to its RT-only shading. Both now require 1.1.
- `TD5RE_SELFTEST_ROWS=<no match>` crashes (NULL access in a system DLL at present 56). Same crash, same
  address, on master's exe: pre-existing, not touched here.

## Is a refactor needed?

**Not for the 3070 target.** The cost sat in four full-res passes with a full-res 3-iteration denoiser each.
Half-res trace + half-res denoise + one cheap upsample removes ~70% of it with no architecture change, and the
measured result leaves a 3070 well inside 60 fps. What a refactor would buy is quality per millisecond and
coverage, not the 3070 fix. Proposed, each needs Mariano's go-ahead:

| Step | What | Why | Size |
|---|---|---|---|
| R1 | Temporal accumulation (reprojection with motion vectors) feeding the a-trous | denoise is the biggest cost; a history buffer would let 1 iteration do what 3 do now, and quarter-res tracing for LOW-RT | large (needs per-pixel motion vectors in the G-buffer) |
| R2 | Pack shadow + GI into one RG16F mask and one denoise | 4 denoise chains -> 3, fewer full-screen passes | medium |
| R3 | Feed the free-roam tiles into the TLAS (per-tile merged BLAS, streamed with the tiles) | RT in the city at all | large |
| R4 | Sub-allocate BLAS from pooled heaps instead of one committed resource per BLAS | fewer driver allocations while streaming (no spikes measured here: BLAS p99 0.13 ms) | medium |
| R5 | Pick the tier with a 1-second RT micro-benchmark at first launch instead of the adapter name | name tables rot; measures the real card | medium |
| R6 | Measure the cold-shader-cache first launch on a 3070 | the one load number not measured | needs the 3070 |

## Auto-detected tier (main.c `td5_perf_rt_class_tier`)

No DXR 1.1 or < 6 GB VRAM -> LIGHTING LOW. RTX 20xx / 30xx (3090 included), 4060, Radeon, Arc -> MEDIUM.
RTX 4070 / 5070 and up -> HIGH up to 4.6 MP (about 1.25x 1440p), MEDIUM above. 4080 / 4090 / 5080 / 5090 ->
HIGH at any size. Unknown family -> HIGH only with >= 16 GB VRAM. `TD5_AUTOPERF_VERSION` 2 -> 3 re-tiers once an
install still on the shipped HIGH/HIGH defaults; customised settings are left alone.
Selftest row `u1-rt-options` checks 11 adapter cases, the live options and that MEDIUM selects half-res.
