# TD5RE Modding Strategy (draft)

Status: **draft**, 2026-06-02. Decision context captured from a strategy conversation while
end-of-cycle bugfixes are still in flight. Revisit once the faithful baseline is locked.

## Decisions on record (user-confirmed)

| Question | Answer | Consequence |
|----------|--------|-------------|
| Mod scope | **All four**: content swaps, new game modes, engine/behavior, visual/rendering | This becomes a moddable *engine*, not just a faithful port. |
| Audience | **Both, eventually** — dev-recompile first, end-user runtime loading later | Design data formats now to be human-editable & versioned, even while only the dev uses them. |
| Faithfulness | **Free to fork** — original-match & replay-compat no longer a product requirement | Faithfulness is demoted from *requirement* → *test oracle*. Keep it as a regression baseline, not a straitjacket. |

## Bottom line

**Do not rewrite. Do not pre-build a plugin framework. Stage a "strangler" evolution.**

A from-scratch rewrite throws away the project's crown jewel — bug-for-bug fidelity that
cost enormous effort (trig LUTs, suspension integrator, checkpoint records, camera profiles,
M2DX slot model). And the thing that actually makes TD5 moddable is **not** the code shape —
it's that **every data format is already decoded** (STRIP.DAT, MODELS.DAT, car/level ZIPs,
TGA, WAV). That survives any refactor. A rewrite re-pays the fidelity cost to buy nothing you
can't get incrementally.

A big up-front refactor (hooks everywhere, externalize every table) is also wrong: you'd be
designing extension seams before you know which mods you want. **Let each real mod pull its
seam into existence**, then generalize. Make one mod, see where it hurts, abstract *that*.

## What you already have (don't reinvent)

- **Loose-file asset override** — `td5_asset.c:257-282`, `:1145-1177` check disk before ZIP.
  Drop a replacement texture/model/sound and it loads. Covers ~80% of content modding *today*
  with zero code changes. This is the foundation for the end-user mod folder.
- **Config + compile-gating discipline** — INI/CLI layer (`main.c:167-330`) and the
  `TD5RE_RELEASE` dev/release split. The mod/faithful split should *reuse this exact pattern*,
  not invent a new one. (See dual-build note: `build_all.bat`.)
- **Trace/replay regression harness** — `race_trace_*.csv`, whole-state snapshots,
  `TD5RE_FORCE_REPLAY`. This is the regression oracle that lets you fork safely (below).
- **Clean module separation** — render decoupled from sim, explicit init order, switch-based
  FSM you can add states to. Globals-based coupling is fine for a single-threaded game loop.

## What blocks deep mods (and how cheap each really is)

The Explore audit rated several of these "blocker," but that was against an *end-user,
no-compiler* standard. For **you, the developer who recompiles**, most are cheap:

| Item | Where | Cost for dev | Cost for end-user loading |
|------|-------|--------------|---------------------------|
| Car table `s_car_zip_paths[37]` | `td5_asset.c:2575` | edit array + recompile (trivial) | needs externalizing to a data file |
| Schedule/pool maps `[19]` | `td5_asset.c:1437-1445` | trivial | needs externalizing |
| Camera profiles | `td5_camera_profiles.h` | trivial | needs externalizing |
| Throttle / tuning tables | `td5_physics.c`, `td5_ai.c` (`g_default_throttle`) | trivial | needs INI/data exposure |
| `MAX_RACERS=6`, `MAX_TRAFFIC=6` | `td5_types.h:78-79` | `#define` bump — **but** faithful AI/checkpoint/replay tables assume 6 | real engineering, not a knob |
| Actor stride `0x388`, fixed-point 24.8 | `td5_types.h:77`, physics | leave alone unless a mod *needs* float | — |

**Honest caveat:** "more racers" and "switch sim to float" are *projects*, not mods — they
break the faithful baseline. Schedule them deliberately (see Phase 3), not casually.

## The fork discipline (the most important idea here)

"Free to fork" does **not** mean "diverge now." The faithful build + trace harness is your
single best debugging tool: it answers *"is this behavior a bug I introduced, or intended?"*
The moment you diverge a subsystem with no test coverage, you lose that answer.

Rule: **a subsystem may diverge from faithful only once it has its own tests.** Until then,
keep faithful behavior behind a flag and trace-diff against it. Cut the cord per-subsystem,
last-responsible-moment — not all at once.

Concretely:
1. Tag the faithful baseline in git the moment bugfixes land (e.g. `faithful-v1`). This is the oracle.
2. Mirror the existing `TD5RE_RELEASE` gating: a `TD5_MOD` build/flag where divergence is allowed.
3. Trace-diff faithful-vs-mod stays green for any subsystem you haven't *intentionally* changed.
4. When a subsystem is fully forked + tested, drop its faithful path.

## Target architecture (end-state shape, not a day-1 build)

```
  ┌─────────────────────────────────────────────┐
  │ Mod layer:  mod folder + load order + manifest│  ← end-user, Phase 2+
  ├───────────────┬──────────────┬───────────────┤
  │ Data layer    │ Game-mode    │ Scripting/hook │  ← Phases 1,4,5
  │ (tables→files)│ registry     │ (behavior)     │
  ├───────────────┴──────────────┴───────────────┤
  │ Core: physics / track / ai / render / sound   │  ← keep; liberalize per-need
  │   (fixed-point sim stays until a mod needs    │
  │    float; render already decoupled)           │
  └───────────────────────────────────────────────┘
```

Design principle: **data over code.** Every hardcoded table you externalize is simultaneously
(a) a content-mod unblock for you and (b) the file format end-users will edit later. Do it once,
human-readable (INI/TOML/JSON — match the existing INI style), versioned from day one.

## Sequencing

**Phase 0 — Lock the baseline (do this before any mod work).**
Finish + merge the in-flight bugfixes. Don't start mod refactors on files you're still patching;
the churn will fight your merges. Tag `faithful-v1`. Snapshot a set of golden trace runs.

**Phase 1 — Data-ize the tables (highest leverage, lowest risk).**
Pick **cars** first as the pattern-setter: move `s_car_zip_paths` + per-car tuning into an
external, documented, versioned data file. This unblocks content mods immediately *and* forces
you to define the schema you'll hand end-users later. Then tracks, camera, checkpoints.
Each table: faithful values become the default data file → behavior is byte-identical → trace stays green.

**Phase 2 — Mod folder + load order.**
Build a `mods/` convention on top of the existing loose-file override: a manifest per mod,
deterministic load order, `[Mods]` INI section. Now you (and eventually end-users) drop content
without recompiling. Still no engine changes.

**Phase 3 — Lift fixed limits where a mod demands it.**
`MAX_RACERS` etc. Real work because faithful tables assume 6 — but now gated by `TD5_MOD`, with
the faithful path intact for regression. Only do this when an actual mod needs >6.

**Phase 4 — Game-mode registry.**
Generalize the `td5_game.c` FSM: a registration seam for new race types/rules/objectives.
Pull the seam shape from 1-2 real new modes you build, don't speculate it.

**Phase 5 — Scripting/extension for end-users.**
A behavior-hook or embedded-script layer so end-users change logic without C. This is the point
where determinism gets hard — decide per-mode whether modded races need deterministic replay.

## Deliberate forks to schedule (not drift into)

- **Fixed-point → float in the sim**: keep fixed-point as long as it works. Flip only when a
  specific mod requires it, per-subsystem, behind `TD5_MOD`. Each flip breaks faithful-match.
- **Replay/save format**: bump the version now and namespace modded saves so modded content
  can never corrupt faithful replays / the golden baseline.
- **Determinism**: decide per game-mode. Scripting + end-user mods make global determinism
  impractical; faithful & competitive modes can keep it, sandbox modes need not.

## What NOT to do

- ❌ Rewrite from scratch.
- ❌ Build a general plugin framework before 2-3 real mods reveal the seams.
- ❌ Flip fixed-point→float or bump `MAX_RACERS` "because you can" — each is a baseline-breaking
  event; gate it and justify it with a concrete mod.
- ❌ Start mod refactors on files with open bugfixes — finish/merge first.
- ❌ Externalize all tables at once — do cars as the pattern, then pull the rest in as needed.

## First low-risk steps (safe to do even mid-bugfix, on a separate branch)

1. `git tag faithful-v1` once fixes merge; archive golden trace runs.
2. Add an empty `[Mods]` INI section + `mods/` folder scaffolding (non-invasive).
3. Externalize the **car** table as the proof-of-concept data format (faithful defaults = byte-identical).
4. Stand up a `TD5_MOD` build flag mirroring `TD5RE_RELEASE`.
