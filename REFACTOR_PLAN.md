# TD5RE — Whole-Game Refactoring Master Plan

Date: 2026-07-09 · Owner: orchestrator session · Executors: parallel `/fix`-style worktree sessions
Derived from 5 read-only audits (repo structure, module health, stubs/dead code, Ghidra-fidelity debt, build/test/config).

## North star (policy change)

**Readability now outranks Ghidra byte-fidelity.** Ghidra (`re/ghidra_export/`) becomes casual
reference only. What replaces fidelity as the correctness contract:

1. **Golden traces** (`trace_goldens.txt`, 2 scenarios × 5 streams: pose/motion/track/controls/progress)
   pin sim behavior. Any sim-touching change must keep hashes identical, or intentionally re-record.
2. **Structure lint ratchets** (extern-in-.c=118, td5_game.h includers=25, warnings=261) only go down.
3. Names, comments, control flow, file layout are free to change. Raw `>>8`/`<<8` is no longer
   blessed style — migrate to `FP_*` macros (exemplars: `td5_physics_assists.c`, `td5_vfx.c`).
   Update the "RE conventions" section of CLAUDE.md when Wave S1 lands.

**Golden-coverage map** (decides HOW to verify each package):
- GUARDED (traces catch drift → refactor freely): physics*(all 5), track, input, AI progress/track, game progress.
- UNGUARDED (add coverage BEFORE restructuring): camera, sound, save, render*, hud, frontend, fe_*, net, vfx.
- Presentation-only (visual check suffices): frontend, fe_*, hud layout, render.

## Parallel-session collision rules

Sessions run in isolated worktrees; collisions happen at merge. Rules:

1. **Choke files — one owner at a time.** Never have two live packages editing:
   `srcs.txt`, `cflags.txt`, `link_libs.txt`, `build_standalone.bat`/`build_all.bat`, `Makefile`,
   `.github/workflows/*`, `scripts/lint_structure_baseline.json`, `.gitignore`,
   `td5re.h`, `td5_types.h`, `td5_game.h`, `td5_platform.h`,
   `td5_frontend_internal.h`, `td5_physics_internal.h`, `td5_render_internal.h`, `wrapper.h`.
   Each package below lists which choke files it owns while active.
2. **One sim module in flight at a time.** Two packages may not both touch a golden-guarded .c —
   goldens can't attribute a hash break to one of two merged diffs.
3. **Broad-touch packages run solo** (marked SOLO): they sweep many files shallowly and collide
   with everything. Schedule them between parallel batches.
4. Re-record goldens only deliberately, in the package that intends a behavior change, never to
   "make the test pass" (`TD5RE_TRACE_GOLDEN_UPDATE=1 pwsh scripts/selftest.ps1 -Suite full`).
5. After any package that adds/removes modules or lowers a ratchet: rebuild `codemap` (automatic
   via build_all) and lower the baseline JSON in the same commit.

Effort: S ≤ half session · M = one session · L = multi-session. Risk: behavior risk, not merge risk.

---

## Wave 0 — Repo hygiene (single session, mostly deletions — DESTRUCTIVE, needs user sign-off)

Not parallel; no code semantics. Space reclaim ~44 GB.

| ID | Package | What | Effort | Caution |
|----|---------|------|--------|---------|
| H1 | Bulk reclaim | Delete `_archive/` (35 G, self-manifested safe), `log/` (8.1 G, regenerated), `re/_retired_dats/` (364 M, dupes of original/ zips) | S | Destructive — confirm each; use the rm wrapper rules (never touch `original/`, `re/assets/`) |
| H2 | Worktree sweep | Remove orphan worktree `.claude/worktrees/fix-1783450069-65856-20049/` | S | **⚠ It hosts branch `fix-1783450069-65856-20049` (street-lights, committed d0129b11, NOT merged, awaiting look-dev). `git worktree remove` only — the branch MUST survive. Do not `-D` the branch.** |
| H3 | Retired-tooling prune | Delete pool-era scripts (`scripts/frida_pool.sh`, `capture_snapshot_pool.py`, `verify_wave2_sweep.py`, `decomp_*_funcs.py`, `analyze_tick59_collision.py`, ttd_record*), ~110 untracked `tools/frida_pool*_*.js`, `tools/cdb_*`, pool-math validators. Archive (don't delete) `ExportAllDecomp.java` + `frida_differential_capture.py` | S | Keep the 13 tracked `/diff-race` tools (see `.gitignore` `!` allowlist — update it in lockstep) |
| H4 | Dead MCP clones | Remove `tools/r2mcp/`, `tools/radare2/`, and `tools/frida-game-hacking-mcp/` **only after verifying `.mcp.json`** (CLAUDE.md still lists frida-game-hacking as a live server — audit and CLAUDE.md disagree; check which is wired) | S | Verify `.mcp.json` first; keep `windbg-mcp/`+`windbg-dlls/` |
| H5 | Docs collapse | Move the 3 superseded frontend spec generations (`re/analysis/frontend_*`, `frontend_diff/ fixlist/ layout/ screens/`, ~85 M) into `re/analysis/archive/`; archive `re/sessions/` (all Mar 2026), dated audits, wave/batch naming reports. Untrack committed tool outputs (`re/tools/frida_main_menu_capture*.txt`, `re/tools/research-archive/*.txt`, `re/scripts/gap_functions_*.txt`) | M | `FRONTEND_SCREEN_GUIDE.md` stays the single authority. Keep `re/analysis/subsystems/`, `formats/`, `ui/` (durable format knowledge) |
| H6 | Strays | Delete 0-byte `TD5.gpr`, `TD5_headless.gpr`, empty `td5mod/-p/`; gitignore the committed `ddraw_wrapper` `.o` files (surface4.o, texture2.o); fix `td5_config.h:11` `TD5RE_FOO` placeholder | S | — |
| H7 | CLAUDE.md refresh | After H1-H6: update directory-layout section, drop pool references, state the new readability-over-fidelity policy | S | Do last in wave |

---

## Wave 1 — Parallel-safe code packages (SIM-SAFE unless noted; run several at once)

Each package owns an exclusive file set. Verification: build_all + selftest smoke (+ goldens where noted, as a tripwire).

| ID | Package | Files owned | Choke files | Effort | Risk |
|----|---------|------------|-------------|--------|------|
| A1 | **Dead-code removal** | delete `td5_frontend_button_cache.c/.h` (retired module, 6 inert stubs) + its assets (ButtonBits.png/BodyText.png) + callers' dead invocations; `td5_frontend.c:2724-2737` no-op `frontend_draw_string`/`_small_string` (zero call sites); `td5_arcade.c:151-155` dead PU enums; `td5_ai.c:88` `RS_DIRECTION_POLARITY_LEGACY` (confirm zero uses); `td5_vfx.c:2400,3642` no-op billboard stub merge; stale "stub" comment at `td5_render.c:3453` | srcs.txt | S | LOW |
| A2 | **Build single-sourcing** | new `wrapper_srcs.txt` + `wrapper_cflags.txt` (kills 4-way dup + `-DWRAPPER_DEBUG` local/CI divergence); collapse `release.yml` into `build.yml` (matrix/reusable workflow); delete legacy `ddraw.dll` target + `ddraw.def` + dead shader-compile step + unused `STRINGS` var; replace build_all's hand-rolled staleness one-liner | all build scripts, workflows, Makefile | M | LOW (verify both exes byte-compare pre/post where flags unchanged) |
| A3 | **ddraw_wrapper restructure** | split `wrapper.h` (1034 LOC god-header, 9 TUs) into per-interface headers; split `d3d11_backend.c` (2595) into device-init / pipeline / draw-submit | wrapper.h, wrapper srcs list (coordinate with A2 — run after A2 lands) | M | LOW (zero game state; visual check) |
| A4 | **Env-knob prune** ~40-60 of ~360 `TD5RE_*` | inline winning branch of permanently-on relics: `*_FIX` set (CAM_NEW, GROUND_CAM_FIX, REPLAY_*, WHEEL_*, GRIP_DRIFT_FIX, BILLBOARD_TREE_FIX, NAMEENTRY/DEMO/SPLIT_LAYOUT/RESULTS/POLICE_AUDIO...), fold legacy A/B fallback branches (`camera.c:242`, `fe_net.c:145/196/321`, `fe_race.c:746`, `save.c:2555/2632`) | SOLO — touches ai/camera/damage/fe broadly | M | MED — default-on so behavior shouldn't change; **goldens must match**; do NOT prune knobs referenced by dev modules until A5 decided |
| A5 | **Dev modules out of release** | `srcs_dev.txt` appended only in dev builds (or file-scope `#ifndef TD5RE_RELEASE`) for selftest/trace/inputscript/pending/profile (+decide benchmark — original feature); makes "instrumentation stripped" true (~3.7k LOC out of shipping exe) | srcs.txt, build scripts (after A2) | M | LOW — release smoke-run; check `-Wl,--whole-archive` interaction |
| A6 | **DAT_ token retirement** | swap ~398 raw `DAT_0045xxxx` source tokens for their existing `g_*` aliases (worst: ai 82, frontend 44, track 27, sound 25, vfx 22); then prune dead #defines from `td5_orig_globals.h` (610 → the ~232 live) | SOLO-ish (many modules, trivial diffs) | M | LOW — compiles identically; goldens as tripwire |
| A7 | **Comment debt** (split by module across sessions) | for the 2178 `[CONFIRMED @ 0xADDR]` sites: keep the addr tag, ADD one-line "what it does". Priority: td5_ai (310), td5_physics (256), td5_game (215), td5_track (203) | none | L (divisible) | ZERO — comments only; ideal filler work; do sim modules BEFORE their Wave-2 restructure |
| A8 | **Warning paydown** | fix mechanical classes: misleading-indentation (58), unused-variable (31), unused-function (18), unused-const/set-variable (18); drop `-Wpedantic` (33 pure noise) from cflags.txt; lower `warnings_total` baseline | cflags.txt, baseline JSON | M | LOW |
| A9 | **fe_* headers** | real `.h` for td5_fe_menu/net/mp_setup/carstats/devscreens/race instead of extern/implicit decls via `td5_frontend_internal.h` | td5_frontend_internal.h | S | LOW |
| A10 | **Shared helpers** | new `td5_bytes.h` (LE read/write: asset.c:773, save.c:1063, inline sites track.c:8623, asset.c:1768); `argb8()` inline (8+ dup sites); unify clampf/clampi/iabs into a math header; drop commented-out `abs_i` | SOLO — shallow cross-cutting | M | LOW — byte-identical helpers; goldens tripwire |

Suggested batches: **Batch 1** = A1 + A2 + A8 + A9 (+A7 filler) in parallel → **Batch 2** = A3 + A5 (+A7) → **Solo slots** = A4, A6, A10 one at a time between batches.

---

## Wave 2 — Golden-guarded sim readability (ONE sim module in flight at a time)

Full ladder every package: build_all → lint → `selftest.ps1 -Suite full` → **goldens identical**.
Do A7 (comments) for a module before restructuring it. Order = dependency + value.

| ID | Package | Scope | Effort | Notes |
|----|---------|-------|--------|-------|
| S1 | **FP_* macro migration, per module** (4 sub-packages, serial): td5_physics.c (158 raw shifts), td5_physics_suspension.c (91), td5_physics_collision.c (98), td5_track.c (90) | mechanical `>>8`/`<<8`/0x1000-angle → `FP_*`/angle macros; macro must be bit-identical (no rounding change) | L (4×M) | Goldens catch any drift instantly — safest possible arithmetic refactor. Also updates CLAUDE.md RE-conventions blessed style |
| S2 | **Decompiler-local renames**: physics.c (23 decls iVarN/local_N — 2×2 solver, suspension forces), collision.c (13 — OBB/impulse), ai.c (23 — span math; promote the comment-names to code) | pure rename | M | Can ride along with S1 per module (same module = same package) |
| S3 | **Split `td5_physics_update_player` (1462 LOC)** into labelled phase helpers; then td5_physics.c module split alignment + `g_actor_pool` ownership move toward td5_game.c (unblocks race_state conversion) | code motion in guarded module | L | The single worst function in the codebase |
| S4 | **td5_game.c**: flatten `frame_run_sim_loop` (1043, 4× `do{}while(true)`) into named-exit loops; continue init_race_* phase extraction (roadmap §3); name the 809 hex magic constants (mode enums, spawn offsets, seeds) | code motion + literal naming | L | Partial golden (progress) — lean on RaceTrace fixed seed A/B |
| S5 | **td5_ai.c split** (11967 LOC → ai_traffic / ai_track / ai_cop / ai_init) + extract cop-chase mutators into new `td5_race_events.h` write-API (roadmap §1 blocker) | module split, guarded | L | Owns td5_game.h + new header while active. Resolve/close `td5_ai.c:3133` FIXME during split |
| S6 | **td5_track.c split**: parser (parse_models_dat, resolve_neighbor) vs runtime traversal (update_position_recursive); resolve `td5_track.c:6239` divergence FIXME (decide: adopt guards or document) | module split, guarded | L | — |
| S7 | **td5_sound event inversion** (advance_sky_rotation → event flag) to unblock its td5_game.h conversion (roadmap §1) | small, isolated | S | SIM-timed — goldens + listen test |

Open TODOs carried (do within the owning package, or explicitly close as WONTFIX in EXPECTED_BEHAVIOR.md):
suspension D3+D4 wheel-attitude (`suspension.c:3020`), damage_lockout switch (`physics.c:5924`),
drivetrain pitch-chain stub (`drivetrain.c:821`), billboard pool (`render_effects.c:3420`),
3 deprecated inner-tick trace stubs (`suspension.c:2291/2486/2827` — delete or reconnect).

---

## Wave 3 — Unguarded modules: coverage FIRST, then refactor

| ID | Package | Scope | Effort | Risk |
|----|---------|-------|--------|------|
| C1 | **Coverage build-out** (prereq for the rest of the wave) | add trace streams for camera + sound to td5_trace.c and record goldens for them; save/load round-trip selftest step; promote the manual net loopback (`td5_net_selftest`) into the suite; fix the known-broken drag natural-finish scenario (`td5_selftest.c:129`) | L | net/save/fmv/radio currently have ZERO net — this is what makes the rest of the wave safe |
| C2 | **td5_camera.c cleanup** | 74 raw shifts → macros, 6 gotos → structured, name preset offsets | M | after C1 camera trace |
| C3 | **td5_hud.c** | fix its 22 extern-in-.c (worst leaker — needs producer headers), split into hud_minimap / hud_overlays / hud_font_atlas, decompose render_overlays (892) + render_minimap (876) | L | visual + screenshot check; lowers extern baseline |
| C4 | **td5_render_mesh.c** | decompose `render_actors_for_view` (1288 LOC, largest non-physics fn) | M | visual check |
| C5 | **td5_fe_race.c split** (9634 → per-screen TUs: CarSelection 872, RaceResults 780, TrackSelection 537, QuickRace, NameEntry, summary) + name the screen-state magic | L | pure UI; screen-walk selftest covers nav |
| C6 | **td5_frontend.c split** (11213, 254 fns → ui_rects render / input poll / resource init) + trim 33 includes | L | after A9/C5 settle internal header |
| C7 | **td5_save.c** | 7 error-path gotos → structured, name field offsets | M | after C1 round-trip test |
| C8 | **td5_sound.c** | FP migration (28 shifts), doppler magic naming | M | after C1 sound trace |
| C9 | **td5_platform_win32.c split** (7234, 229 fns, 145 statics → window/input, render-preset, filesystem, audio-device) | L | platform-only, selftest smoke |

Parallelism: C2/C3/C4/C5 are disjoint file sets — safe concurrently once C1 lands. C6 after C5.

---

## Wave 4 — Structural core (high blast radius; serialize; last)

| ID | Package | Scope | Effort |
|----|---------|-------|--------|
| G1 | **g_td5 shrink round 1** | migrate benchmark+trace fields to module-owned structs (roadmap-endorsed first cut, dev-only); then per-module `const TD5_XxxConfig* td5_config_xxx()` views starting with safe ini blocks (volumes, laps) | M+M |
| G2 | **td5_types.h decomposition** (852 LOC, 76 defs, 27 includers → math / actor / render / game type headers) | M — owns td5_types.h solo |
| G3 | **td5_game.h → td5_race_state.h migration** for remaining includers + extern paydown campaign (118 → target <60; hud 22 + game 16 first); ratchet baselines down as you go | L — after S5/S7 unblock ai+sound |
| G4 | **Config-system consolidation** | schema table generating the 108-entry CLI table + `--Help` from one source (kills td5re.h / main.c:351 / help drift) | M |

---

## Standing decisions (user)

1. **Wave 0 deletions** need explicit sign-off (esp. H1's 35 G `_archive/` and H2's worktree — street-lights branch must survive).
2. **A5 benchmark module**: compile out of release or keep (it's an original-game feature)?
3. **Env-knob prunes that are behavior commitments** (A4 legacy A/B branches): each fold locks in the new behavior permanently — batch-approve the list before the session runs.
4. Golden re-records: none planned; any package requesting one must justify it.

## Orchestration bookkeeping

- Claim a package by editing the Status ledger below on **master** (commit the one-line claim BEFORE
  starting work, so parallel sessions see it): `todo` → `wip <session/branch>` → `done @<sha>` (or
  `skipped <reason>` / `blocked <reason>`).
- Every merged package: goldens status, ratchet numbers after, codemap regenerated.
- This plan extends `STRUCTURE_ROADMAP.md` (which stays the layering/ratchet authority); on conflict, roadmap wins for architecture direction, this file wins for sequencing.

## Status ledger

Execution order = top to bottom. A package may only be claimed if: (a) everything it depends on is
`done`, (b) no `wip` package owns an overlapping choke file, (c) for Wave-2/sim packages, no other
sim package is `wip`.

| ID | Package | Status | Notes |
|----|---------|--------|-------|
| H1 | Bulk reclaim (_archive, log, _retired_dats) | todo | NEEDS USER SIGN-OFF |
| H2 | Orphan worktree sweep (keep street-lights branch!) | todo | NEEDS USER SIGN-OFF |
| H3 | Retired-tooling prune | todo | NEEDS USER SIGN-OFF |
| H4 | Dead MCP clones (verify .mcp.json first) | todo | NEEDS USER SIGN-OFF |
| H5 | Docs collapse | done @87db7dd5 | docs-only; goldens N/A; smoke 12/12 PASS; ratchets unchanged (extern=118, game_h=25); INDEX.md links repointed |
| H6 | Strays (0-byte gpr, -p dir, .o files) | done @53dc2bd8 | other strays already gone from tree; only td5_config.h placeholder fixed; smoke 12/12 PASS |
| H7 | CLAUDE.md refresh | todo | after H1-H6 |
| A1 | Dead-code removal | done @7c887736 | full suite 46/46 PASS, both golden hash sets match; ratchets unchanged (extern=118, game_h=25); arcade PU item was already resolved pre-existing |
| A2 | Build single-sourcing | done @e7bc521a | wrapper_srcs/cflags.txt + reusable CI workflow + ddraw.dll/def/STRINGS removed + staleness check extracted; build_all verified in worktree AND main repo (byte-identical exe size); CI YAML unverified until next push |
| A8 | Warning paydown | todo | |
| A9 | fe_* headers | todo | |
| A3 | ddraw_wrapper restructure | todo | after A2 |
| A5 | Dev modules out of release | todo | after A2; user decision on benchmark |
| A4 | Env-knob prune | todo | SOLO; user batch-approval of fold list |
| A6 | DAT_ token retirement | todo | SOLO |
| A10 | Shared helpers (td5_bytes.h, argb8, math) | todo | SOLO |
| A7 | Comment debt (per-module, divisible) | todo | filler; sim modules before their Wave-2 package |
| S1a | FP_* migration td5_physics.c (+S2 renames) | todo | sim-exclusive |
| S1b | FP_* migration td5_physics_suspension.c | todo | sim-exclusive |
| S1c | FP_* migration td5_physics_collision.c (+renames) | todo | sim-exclusive |
| S1d | FP_* migration td5_track.c | todo | sim-exclusive |
| S3 | update_player split + physics.c alignment | todo | sim-exclusive; after S1a |
| S4 | td5_game.c flatten + magic naming | todo | sim-exclusive |
| S5 | td5_ai.c split + td5_race_events.h | todo | sim-exclusive; ai renames ride along |
| S6 | td5_track.c parser/runtime split | todo | sim-exclusive; after S1d |
| S7 | td5_sound event inversion | todo | sim-exclusive |
| C1 | Coverage build-out (camera/sound traces, save/net tests) | todo | prereq for C2/C7/C8 |
| C2 | td5_camera.c cleanup | todo | after C1 |
| C3 | td5_hud.c extern fix + split | todo | |
| C4 | render_actors_for_view decompose | todo | |
| C5 | td5_fe_race.c per-screen split | todo | |
| C6 | td5_frontend.c split | todo | after C5 |
| C7 | td5_save.c goto cleanup | todo | after C1 |
| C8 | td5_sound.c FP + doppler naming | todo | after C1 |
| C9 | td5_platform_win32.c split | todo | |
| G1 | g_td5 shrink round 1 + config views | todo | |
| G2 | td5_types.h decomposition | todo | solo on td5_types.h |
| G3 | td5_game.h migration + extern paydown | todo | after S5, S7 |
| G4 | Config-system consolidation | todo | |
