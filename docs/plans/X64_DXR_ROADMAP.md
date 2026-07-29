# x64 / DXR roadmap

Status as of **2026-07-28**. Working branch `x64-stage1-sse2` (5 commits, not pushed).

## Why any of this exists

The goal is optional raytraced (DXR) shadows. It is blocked by exactly one thing, measured
rather than assumed:

| Process | RTX 5070 Ti | WARP (control) |
|---|---|---|
| 32-bit | **RaytracingTier 0 — NOT SUPPORTED** | 11 (TIER_1_1) |
| 64-bit | **RaytracingTier 12 (1.2)** | 11 (TIER_1_1) |

Same GPU, same driver, same source file — only process bitness differs. WARP returning 1.1 in
BOTH proves the query path and the 32-bit D3D12 runtime are fine: **NVIDIA withholds DXR from
32-bit processes.** `ID3D12Device5` still creates successfully in 32-bit, so a probe that only
checks device creation will mislead you.

TD5RE builds `i686-w64-mingw32`. So: **64-bit migration → then DXR.** The
D3D11On12-vs-full-D3D12-rewrite question is downstream of a prerequisite neither option
addresses.

Staging principle: **only ONE stage may break the golden traces.** Everything else is
sequenced so the goldens stay usable as the correctness net.

## Stages

| Stage | Work | Status |
|---|---|---|
| **0** | Layout `_Static_assert`s on disk/wire structs | ✅ `d3d191ce` (on master) — verified |
| **1a** | x87 trig LUT → `lrintf` | ✅ `4b732465` — bit-identical, goldens green |
| **1b** | SSE2 float math + portable `TD5_F32_SPILL` | ✅ `323ffb8c` — **merge-gated** |
| **2** | Pointer widening (still 32-bit) | 🔄 step 1 done `9aee8c3a` |
| **3** | Flip `-m32` → `-m64` | ⬜ ~half a day, build-side only |
| **4** | DXR renderer (D3D11On12 vs D3D12 — decide then) | ⬜ unreachable until 3 lands |

### Stage 1a — bit-identical, no behaviour change
Replaced the hand-written x87 LUT builder (`fnstcw`/`fldcw` PC=64 + `flds/fmuls/fistpl`) with
`(int)lrintf(v * 4096.0f)`. Verified identical for all 5120 entries under BOTH `-mfpmath=387`
and `-mfpmath=sse` (FNV `364a36a3dee9c686`). Exact, not lucky: 4096 is a power of two so the
multiply never rounds at any precision, and `lrintf` matches FISTP's default round-to-nearest-even.
Port inline-asm sites: 5 → 1.

### Stage 1b — the one stage that moves the goldens
Measured on an identical seeded race, with a same-binary control that came back IDENTICAL
(the harness `frame` column is wall-clock and must be excluded — which is why
`trace_goldens.txt` hashes rows "frame column stripped").

- First divergence is a SINGLE field: `motion` tick 2, `vel_y` −64 → −192 — one 24.8 quantum.
- Everything after is **chaotic amplification, not regression**: AI `controls` diverge at tick
  ~424, `track` ~431, then it is simply a different race. Inherent to a chaotic AI racing sim.
- `sound` bit-identical; in the pinned golden scenario `track`/`controls`/`progress`/`sound` all
  still match ⇒ **the 24.8 fixed-point core is untouched**.
- Render impact negligible: `rgold` moscow worst-cell L1 21 → 25 against a limit of 4000.

⚠️ **An x87 build and an SSE2 build cannot share replays or play netplay together.** (32-bit ↔
64-bit protocol interop still holds — but only if both ends are SSE2.)

### Stage 2 — pointer widening
Full detail in `X64_STAGE2_POINTER_WIDENING.md`. Safety property: pointer width does not change
arithmetic, so **goldens must stay GREEN throughout** — they are the proof the rework is correct.

| Step | Status |
|---|---|
| Route/script handles (`RS_*_PTR`) | ✅ `9aee8c3a` |
| Camera `int actor` API (10 sigs + 30 call sites) | ⬜ |
| Delete redundant `g_actorBaseAddr` | ⬜ cheapest win |
| `g_spanTable` / `g_vertexTable` retype | ⬜ |
| Mesh headers — **option C** (split disk/runtime) | ⬜ the bulk, ~63 sites |
| `offsetof` sweep + `TD5_ACTOR_STRIDE = sizeof` | ⬜ |
| `td5_save.c` on-disk stride | ✅ field-mapped — "truncate" rested on a false premise |

What is NOT a problem, contrary to first impressions: saves (text INI), netplay (all-fixed-width
wire structs — 32- and 64-bit clients interoperate), replay (pointer-free, memory-only),
`td5_msvc_rand.c` (pure `uint32_t` LCG, bit-identical), the Win32 API surface, calling
conventions, and the wrapper's M2DX code-patch (dead in standalone `td5re.exe`; `#ifdef` it out).

## MEASURED: 45 of 67 modules already compile clean on x86_64

A portable x86_64 MinGW-w64 (GCC 16.1.0 — the SAME version as the bundled i686 one, so
diagnostics are pure arch effects, not compiler-version noise) now lives at
`td5mod/deps/mingw64/`, gitignored. A compile-only pass over `srcs.txt` gives the definitive
remaining worklist:

- **45/67 modules compile clean today.**
- 22 failures / 445 errors reduce to **two causes**: the `TD5_Actor` layout (435 assertion
  failures) and `main.c`'s CONTEXT registers (10 — **fixed**).
- **Zero truncating-pointer errors.** `-Werror=int-conversion` and
  `-Werror=incompatible-pointer-types` were active and found nothing — Stage 2's committed
  pointer work (route/script handles, camera API, `g_actorBaseAddr`, span/vertex tables) already
  cleared that class.
- Root cause of all 435: `sizeof(TD5_Actor)` 0x388 -> 0x398 (four `void*` at +0x1B0..+0x1BC gain
  16 bytes), shifting every field after it. **Not one assert below +0x1B0 fired** — the
  predicted boundary, confirmed mechanically rather than argued.

Blind spot: the mesh-header `uint32_t` fields produce NO compile errors — storing a truncated
pointer there is valid C that fails silently at RUNTIME above 4 GB. That class needs the golden
net, not the compiler, which is why the drag golden was added.

Also note this makes Stage 3 smaller than scoped: the build-side work stands, but the SOURCE
side is one root fix plus fallout.

Rerun with `scratchpad/x64_trial.ps1` after each Stage 2 step to watch the list shrink. It runs
nothing, so it is immune to the `nvwgf2um.dll` fault that blocks golden verification.

## The blocker

**`nvwgf2um.dll` faults during `Present` at `race-moscow-base`**, before the suite reaches the
golden scenarios. **Pre-existing** — identical instruction offset, stack, scenario and null read
appear in `log/crash.baseline_prev.log` from 2026-07-27, predating all of this work.

It gates runtime verification for everything: Stage 1b's merge, the batching work, and the rest
of Stage 2.

**It looks CODE-RELATED, not ambient GPU state** (corrected later the same day — the first read
blamed session-long hardware degradation, which was wrong). The support is locality: a null
dereference at a FIXED instruction, reproducing across branches and resolutions. Thermal/driver
decay does not land on one instruction every time.

⚠️ **Corrected 2026-07-28 by n=4.** Two thirds of that locality argument does not survive
measurement. The scenario is NOT fixed and the frame window is NOT narrow:

| run | outcome | scenario | present | `device_generation` |
|---|---|---|---|---|
| 1 | clean 55/55 | — | — | — |
| 2 | crash | `race-newcastle-circ` | 5208 | 7 |
| 3 | crash | `race-moscow-base` | 3285 | 2 |
| 4 | crash | `race-moscow-rep2` | 4062 | 3 |

Three DIFFERENT scenarios and a 3285–5208 present spread. Only the instruction offset (`0x25CBE1`)
is actually invariant. So "`race-moscow-base` is what selects the fault" is **refuted** — it was an
artefact of it being the first heavy scenario the suite reaches, i.e. sampling bias, and a bisect
by scenario knob would have chased nothing.

`device_generation` 7 / 2 / 3 is the more useful signal: each fatal crash is the END of a
TDR-recovery sequence (6, 1 and 2 prior recoveries), so how far a run gets is a function of how
many recoveries it survives. That reframes the target from "which scenario triggers it" to "why
recovery is not durable" — and connects it to the device-lost recovery leak parked below.

Lowering resolution raises the completion rate: 4 of 5 full-suite runs completed at 960x507 or
1280x676, versus repeated failures earlier. Still a mitigation, not a fix.

Established:
- **Not invalid API usage.** A full run under `TD5RE_D3D_DEBUG=1` produced ZERO
  CORRUPTION/ERROR/WARNING validation messages — only after-the-fact `DEVICE_HUNG`. Valid calls
  doing too much work, not malformed ones.
- **Not draw volume.** `max_icount`/`max_vcount` are running maxima, not caps.
- **Not the sun shadows.** A 3-runs-per-config trial: `SunShadows=1` → 5 events, 3/3 runs
  affected; `SunShadows=0` → 4 events, 2/3 runs affected. Indistinguishable. (An earlier n=1
  comparison showed 0 for shadows-off and looked decisive — it was luck.)
- **No CSM in this tree** (parked on `csm-shadow-wip`). The shadow path here is a per-pixel
  march, `TD5RE_SHADOW_STEPS` default 24 — investigated and cleared by the trial above.

Open: **no code path is identified yet**, and TDR occurrence is PROBABILISTIC — run-to-run
variance swamps single-run comparisons. Any further bisection needs 3+ runs per configuration;
smoke (~25 s) plus the TDR-log line delta is the cheap harness. Candidate next steps: bisect by
scenario knob (traffic / cops / opponents / fast-forward) rather than by render feature, since
`race-moscow-base` is what selects the fault and standalone track-0 runs reached ~41k presents
without it.

Reduced resolution lowers the probability but does not eliminate it — a mitigation, not a fix.

Two commits carry an explicit "do not merge, verify first" marker: `323ffb8c` and `9aee8c3a`.

## Parked, not blocking

- **Device-lost recovery leak** — ~64 MB and ~100 handles retained per device recreation,
  quantified 2026-07-28. Real, app-side, unticketed. Also the reason a `degrade-private-bytes`
  FAIL should be triaged by checking `log/gpu_device_lost.log` FIRST.
- **Draw-call batching** — `DRAW_BATCHING_PLAN.md`. Explicitly not a crash fix.
- **`rgold-race-golden-pelton`** — was already failing on master pre-change; rebaselined into
  Stage 1b, root cause never diagnosed.
