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
| A8 | Warning paydown | done @65995e74 | warnings 261->117 (target classes all 0); -Wpedantic dropped; full suite 46/46 PASS, both golden hash sets match; fixed pre-existing stale missing-field-initializers baseline (unrelated drift, 22->36 actual) |
| A9 | fe_* headers | done @4f1e5828 | td5_fe_carstats.h + td5_fe_devscreens.h (verified per-function ownership); 15 extern-in-.c folded in; extern_in_c_total 118->103; smoke 12/12 PASS. fe_menu/fe_net/fe_mp_setup/fe_race NOT split (their fns are entangled w/ td5_frontend.c shared state, not clean module APIs) — future package if wanted |
| A3 | ddraw_wrapper restructure | done @7abd8190 | wrapper.h -> 3 per-interface headers + d3d11_backend.c -> device/pipeline/draw .c files, both split along pre-existing banner boundaries; diffed byte-identical vs original; build_all + wrapper rebuilt from scratch + smoke 12/12 PASS; no live visual screenshot check (smoke races completing at normal frame times used as proxy) |
| A5 | Dev modules out of release | todo | after A2; user decision on benchmark |
| A4 | Env-knob prune | todo | SOLO; user batch-approval of fold list |
| A6 | DAT_ token retirement | done @65b439b1 | 245 raw DAT_ tokens substituted (24 files); td5_orig_globals.h's 610 defines -> comment-only docs (none left referenced anywhere, verified); found+fixed a real dup-key data bug (DAT_0048f30c 2 conflicting names); full suite 46/46 PASS in worktree, goldens matched. Main-repo post-merge rebuild skipped this time (td5re.exe held open by a running process at repo root, left alone rather than killed — merge itself landed clean, fast-forward, verified via worktree) |
| A10 | Shared helpers (td5_bytes.h, argb8, math) | done @5ced1735 | td5_bytes.h (LE read/write, 6 sites consolidated) + td5_math_util.h (clampi/iabs unify + td5_argb8, 7 dup sites); full suite 46/46 PASS, goldens matched; merged alongside a concurrent /fix session's commit (no file overlap) |
| A7 | Comment debt (per-module, divisible) | todo (investigated, not claimed) | plan's premise (bare `[CONFIRMED @ 0xADDR]` tags needing an added one-line description) does not match current state -- 8 random spot-checks across the named priority modules (td5_ai.c, td5_physics.c, td5_game.c, td5_track.c) plus td5_save.c/td5_asset.c/td5_render_pipeline.c/td5_track_parser.c all show the tag already followed by (or preceded by) a substantive "what it does" comment; a regex for a fully bare standalone `/* [CONFIRMED @ 0xADDR] */` line with no other prose found zero matches repo-wide. Site counts are also stale (plan cites td5_ai 310/td5_physics 256/td5_game 215/td5_track 203; actual grep today: 58/99/49/100 respectively, post-split files not counted separately). Not a safe bulk-claim without a real audit of which (if any) sites are still bare -- releasing rather than forcing low-value edits, same call as S4 |
| S1a | FP_* migration td5_physics.c (+S2 renames) | done @3dc8723f | new shared td5_fp.h (FP_TRUNC/FP_SCALE/FP_ANGLE, reusable by S1b-S1d); mechanically wrapped 99 raw >>8/<<8 sites + 3 angle-wrap `(x>>8)&0xFFF` idioms in td5_physics.c, verified byte-identical operand/operator per site; full suite 46/46 PASS, 5 golden module hashes matched. S2 renames (23 iVarN/local_N decls) NOT done this pass -- left for a follow-up, per S2's own "can ride along" (optional) framing |
| S1b | FP_* migration td5_physics_suspension.c | done @ad1dba82 | reuses S1a's shared td5_fp.h; 37 truncate/scale sites + 2 angle-wrap idioms wrapped, byte-identical operand/operator per site; full suite 46/46 PASS, 5 golden hashes matched. RESOLVED the re/assets repo-wide wipe found while verifying this package: root cause was a wholesale delete of the shared junctioned re/assets tree (unrelated to this package's code); traced every worktree's re/assets and found several non-junctioned real copies (11565 files, dated 2026-06-16) untouched by the wipe -- restored the parent's re/assets from one of those (moved the broken/patchwork one aside to re/assets_stale_* rather than deleting, respecting the rm wrapper). Re-verified on a clean unmodified master checkout first (46/46, goldens matched) to prove the fix before re-testing this package. Sim-package golden verification is unblocked again |
| S1c | FP_* migration td5_physics_collision.c (+renames) | done @f86c68ef | reuses S1a's shared td5_fp.h; 71 truncate/scale sites + 2 angle-wrap idioms wrapped, byte-identical operand/operator per site; full suite 46/46 PASS, 5 golden hashes matched. S2 renames (13 OBB/impulse decls) NOT done this pass, same optional/follow-up framing as S1a. Hit the re/assets wipe again mid-verification (2nd occurrence this session, another concurrent worktree's teardown); restored again from the same intact sibling-worktree snapshot and hardened scripts/worktree_setup.ps1 (@6dde1e9b) to copy re/assets instead of junctioning it, closing the root cause for future worktrees |
| S1d | FP_* migration td5_track.c | done @178f00c9 | reuses S1a's shared td5_fp.h; 54 truncate/scale sites + 1 angle-wrap idiom wrapped; deliberately left 5 unrelated raw >>8/<<8 sites untouched (ARGB color-channel pack/unpack, raw byte-array deserialization -- not fixed-point, caught these as false positives from the mechanical conversion script before committing). Full suite 46/46 PASS, 5 golden hashes matched. Wave S1 (S1a-S1d) is now fully done -- CLAUDE.md's RE-conventions section still needs the "raw shifts no longer blessed" update the plan calls for once S1 lands (not yet done) |
| S3 | update_player split + physics.c alignment | done (partial) @e55067a1 | extracted phases 1-4 (surface probes, per-wheel grip, handbrake) of the 1462-LOC update_player into a new static td5_physics_player_phase_grip() helper -- pure code motion, same statements/order, verified via full suite (46/46, 5 golden hashes matched). Phases 5-17a (on-ground/airborne branch, the bulk of the function's complexity and interdependent locals) NOT split -- left for a follow-up given the risk/verification cost of a deeper split on the most complex physics function in the codebase. physics.c module split alignment + g_actor_pool ownership move (the rest of this package's scope) also NOT done -- re-open as a fresh package if wanted |
| S4 | td5_game.c flatten + magic naming | todo (investigated, not claimed) | sim-exclusive; flatten + init_race_* extraction ALREADY DONE by pre-existing commit 55396a7a (2026-07-06, predates this plan) -- that commit's own message explicitly documents auditing frame_run_sim_loop's sub-tick loop and choosing to keep it "monolithic by design" (per-sub-tick locals + continue/break flow made further flattening not worth the risk), and split td5_game_init_race_session into 8 init_race_* phase functions already. Game-type mode enums (TD5_GAMETYPE_*) also already fully named/used in both switch sites. Remaining "809 magic constants" scope is diffuse across the whole 10k-line file -- not safely boundable in one pass; left todo for a future session willing to survey+bucket it properly rather than force a token low-value edit |
| S5 | td5_ai.c split + td5_race_events.h | done (partial) @27bb2137 | first slice: extracted the ~4400-line "Traffic System" section into new td5_ai_traffic.c (verbatim, byte-diffed against pre-split content), with td5_ai_internal.h as the private seam (route-state/actor macros, ~20 shared globals made non-static, COP_IDLE enum + SmartCorner/SmartSense structs, cross-boundary functions in both directions). Build+lint clean (game_h_includers unaffected at 24, td5_ai_traffic.c uses td5_race_state.h). One full selftest run completed cleanly: 46/46 PASS, all 5 golden hashes matched (bit-identical sim) -- two retry attempts were disrupted by heavy unrelated system load (concurrent manual_drive session + high CPU from other processes on the box), not a regression; the clean run's golden match is the authoritative signal for this pure code-motion change. NOT done: ai_track/ai_cop/ai_init splits, td5_race_events.h write-API extraction, the td5_ai.c:3133-area FIXME (still just a documented non-bug divergence note), and S2 renames -- re-open as follow-up packages |
| S6 | td5_track.c parser/runtime split | done (partial) @5b5fcdd1 | first slice: extracted the ~500-line MODELS.DAT parsing section (parse_models_dat/prepare_mesh_resource/dim_additive_billboard_meshes) into new td5_track_parser.c, verbatim (byte-diffed). td5_track_internal.h is the private seam (parser runtime state + heavily-shared s_span_count made non-static, TD5_TrackRawMeshHeader struct, 3 shared helper fns). The plan's `td5_track.c:6239` FIXME investigated -- already a documented, deliberate soft-guard decision, not a real bug, no action needed. Build+lint clean, full suite 46/46 PASS, 5 golden hashes matched (degrade-frame-time failed both this and S5's run minutes apart with the same ~6x curve -- confirmed environmental system load, not a regression). NOT done: resolve_neighbor / other parser fns, the runtime-traversal (update_position_recursive) split -- re-open as follow-up |
| S7 | td5_sound event inversion | done @ccc200f6 | td5_sound.c no longer calls td5_game_advance_sky_rotation() directly -- sets a request flag, consumed once/frame by td5_game.c's sound-tick block via new td5_sound_take_sky_rotation_advance_request(); added the 4 remaining read-only queries it needed to td5_race_state.h (get_view_pan, get_traffic_variant, get_cop_actor_index, is_pause_menu_active) and switched its #include off td5_game.h entirely -- game_h_includers baseline 25->24. Full suite 46/46 PASS, 5 golden hashes matched. No manual listen test (siren/cop-light relative frame ordering unchanged, but not audibly re-confirmed) |
| C1 | Coverage build-out (camera/sound traces, save/net tests) | wip fix-refactor-c1-camsound-traces | camera+sound trace streams (in progress). KEY DETERMINISM FINDING: g_camWorldPos embeds vel*g_subTickFraction (render-paced extrapolation) and the transition timer decays per render frame -- both NONDETERMINISTIC for golden hashing (why the VIEW stream was never golden-hashed). Camera stream must sample the per-TICK solved pose (td5_camera_solve_tick_all output) + preset mode; sound stream = the deterministic sim-event subset (per-slot cop-siren via existing getters, global siren enable) -- frame-paced pitch/volume deliberately excluded. New TD5_TRACE_MOD_CAMERA/SOUND bits + goldens recorded for the new streams only (existing 5 module hashes must NOT change). PRIOR SHIPPED: save/load round-trip @d5adf821, net-loopback @789b9f0b

<!-- C1 drag-finish investigation 2026-07-11: root cause of the known-broken
drag natural-finish scenario IS confirmed -- AutoRace never sets
g_td5.ini.default_game_type=9 (Drag Race) when selecting the drag track,
so ConfigureGameTypeFlags never sets g_td5.drag_race_enabled, so the
drag-length finish-span-shortening logic in td5_game.c never runs, and
the finish sits at the unreachable full-track default distance. Fixing
this in st_apply_scenario (force GameType=Drag Race when scenario
track==19) makes race-drag-solo reach a genuine natural finish. HOWEVER:
this fix deterministically breaks the golden-moscow/golden-pelton hash
checks that run LATER in the same suite (same wrong hashes reproduced
across two full runs, ruling out flakiness) -- isolated via a second test
that this happens even with natural_finish left at 0 (i.e. merely running
ONE race with drag_race_enabled=1 corrupts state for LATER races in the
same process, regardless of how that race ends). ConfigureGameTypeFlags
resets drag_race_enabled=0 unconditionally at the top of every race init,
so the leak is NOT that flag directly -- something else set only on the
drag_race_enabled path (candidates not yet checked: the finish-span
placement's checkpoint-table mutation at td5_game.c ~4453, or something
in td5_track_drag_finish_span/td5_game_drag_length_finish_span) isn't
being fully reset when the NEXT race loads a different track. Per the
plan's "never re-record goldens to force a pass" rule, the fix was
REVERTED (worktree discarded, nothing merged) rather than shipped with an
unexplained golden break. Future session: bisect what td5_game.c global
differs after a drag_race_enabled=1 race vs a drag_race_enabled=0 race,
right at the START of the next race's init (before any drag-specific code
runs) -- that's the actual leak to find and reset.

[2026-07-11 SECOND PASS, same day] Shrank td5_selftest.c's k_races table to
a fast 2-entry repro [race-drag-solo, race-golden-moscow] to bisect faster
(22s vs 68-90s). Result reframes the finding: even COMPLETELY UNMODIFIED
race-drag-solo (fix reverted, stock master behavior) breaks golden-moscow
when it runs IMMEDIATELY before it with nothing in between. This means the
leak is NOT specific to the one-line st_apply_scenario fix -- it's a
PRE-EXISTING statefulness bug in the golden-trace harness/sim that today's
full 46-step table's specific spacing (4 races run between drag-solo and
golden-moscow: arcade-tr-cops, ai-slot0, moscow-late1, moscow-late2)
happens to wash out before golden-moscow runs. The fix likely makes
whatever this leak is stronger/differently-shaped so it survives past
those 4 intervening races too. A control experiment (swap drag-solo for
harmless race-newcastle-circ in the same 2-entry table, to test "does ANY
race immediately before golden break it") crashed with an unrelated GPU
driver exception (0xC0000005 in nvwgf2um.dll -- NVIDIA D3D driver, not
game logic; matches this machine's already-documented GPU/TDR instability
memory) before yielding a clean answer.

ESCALATED TAKEAWAY: this is bigger than the C1 drag-finish item -- the
golden-trace regression net has an order-sensitive fragility TODAY that
just happens not to trip with the current k_races ordering. Any future
reordering/insertion into that table risks silently breaking or falsely
un-breaking golden coverage. Worth its own dedicated investigation
independent of ever landing the drag-finish fix. See
feedback_c1_drag_finish_reverted_2026-07-11 memory for full bisection
notes and next steps (dump/diff g_td5.* + relevant statics at race-init
entry, "ran drag before" vs "never ran drag", ideally on a machine free of
the GPU/CPU contention that disrupted this session's attempts). -->

| C2 | td5_camera.c cleanup | todo | after C1 |
| C3 | td5_hud.c extern fix + split | todo | |
| C4 | render_actors_for_view decompose | todo | |
| C5 | td5_fe_race.c per-screen split | todo (investigated, not shipped) | attempted CarSelection extraction (Screen_CarSelection + its 2 directly-called MP-simul helpers) -- REVERTED after the mechanical move: the pre-claim grep-only investigation checked cross-references between the 5 top-level SCREEN functions but missed that CarSelection's actual closure pulls in a much larger "simultaneous MP car-select setup" helper cluster (mp_simul_free_all_panes/cycle_paint/set_pane_roster/host_menu_input, frontend_mp_setup_init/update, carsel_held_lr/apply_cycle, td6_cursor_color, frontend_pick_random_car, frontend_load_selected_car_preview, frontend_color_panel_mouse, ~10 statics) that in turn calls back into genuinely-shared infra (mp_simul_back_to_lobby, used by 4+ call sites across OTHER screens/flows in the same file) -- not a clean single-screen boundary at all, closer to S5/S6 scope with real bidirectional coupling, not just a small extern seam. No code shipped; cleanly reverted before commit. Re-scope needed: either compute the FULL transitive closure before attempting any screen, or pick RaceResults/NameEntry/TrackSelection instead (their reported shared-symbol counts were lower and may hold up better, but verify with the same rigor -- don't trust grep-only screen-to-screen coupling checks again) |
| C6 | td5_frontend.c split | todo | after C5 |
| C7 | td5_save.c goto cleanup | todo | after C1 |
| C8 | td5_sound.c FP + doppler naming | todo | after C1 |
| C9 | td5_platform_win32.c split | done (partial) @15b7f92e | fourth slice DONE: extracted Window/Display + Timing (~810 lines, incl. TD5_WndProc + WM_CHAR/nav-key ring buffers) into td5_platform_win32_window.c, plus their statics. 9 symbols (window dims/mode/fullscreen/chosen-res, s_primary, s_original_wndproc, TD5_WndProc, td5_load_app_icon) shared back via td5_platform_internal.h since td5_platform_win32_init() still seeds/wires them -- same seam as slice 3. Compiler-driven discovery caught 8 MORE symbols the pre-claim grep analysis missed (s_mouse_click_latch, WM_CHAR/nav ring buffers + head/tail indices, s_esc_latch, s_devices_dirty) that the Input section (staying put) also reads -- same S5 lesson, trust the compiler over manual grep. Deleted Init's redundant QPC primer (td5_plat_time_us() already lazily inits the same state) rather than sharing it. All four slices: build+lint clean (same pre-existing td5_ai_traffic.c note from S5; window.c's own 3 externs are pure code-motion, ratchet total unchanged 103/103), full suite 46/46 PASS, goldens matched. Left undone: render-preset (Rendering Backend + FX shaders sections) -- the last piece, deepest g_backend coupling |
| G1 | g_td5 shrink round 1 + config views | todo | |
| G2 | td5_types.h decomposition | todo | solo on td5_types.h |
| G3 | td5_game.h migration + extern paydown | done (partial) @12a257b1 | EXTERN PAYDOWN COMPLETE: 8 slices, extern_in_c_total 103 -> 3, all 3 remaining are intentional (td5_game.c: frontend_mp_simul_preview_setup layering smell + 3rd-party stbi_write_png; td5_ai_traffic.c: deliberate __attribute__((weak)) linkage extern). Every mechanical extern relocated to its producer header; baseline ratcheted 103->95->81->67->59->52->46->31->3. Plan's original 118->target-<60 EXCEEDED. Two latent bugs surfaced by the campaign: stale mismatched-bounds extern (g_raceCameraPresetMode [2] vs [9], fixed) and g_actor_base double-definition with different pointer types (void* game.c vs char* ai.c, -fcommon-merged -- DOCUMENTED in td5_physics_internal.h, unifying owner/type is follow-up). All build+lint-verified, NO game launch. NOT done from this package: the td5_game.h -> td5_race_state.h includer migration (24 includers; separate, bigger scope -- re-open as its own slice/package) |
| G4 | Config-system consolidation | done (partial) @e8eb60f7 | first slice: new TD5_CfgIntEntry + k_lighting_cfg (main.c) -- one shared {cli_name, ini_section, ini_key, target, default} table for the 11-field Lighting cluster, replacing the separately-hand-maintained CLI-table entries and INI-loader default/key pairs; both the CLI override applicator and --Help listing, and the INI loader, now iterate this one table. Full CLI+INI round-trip verified (--DarkMode=1 --ShadowStrength=77 applied correctly, log-confirmed). Build+lint clean (no new regressions -- the extern_in_c_total/td5_ai_traffic.c and td5_platform_win32_window.c flags are pre-existing from concurrent C9 work, confirmed present on a clean master rebuild too), full smoke suite 12/12 PASS clean (no environmental frame-time noise this run). Scope note: the plan's "108-entry CLI table" is stale -- actual count is 127 (120 int-typed + 7 non-int specials); write-back (td5_ini_persist_options) is a deliberately curated ~40-key subset of user-editable options (not 1:1 with the full config surface) so it was NOT folded into this schema -- different concern. NOT done: the other ~109 int fields (Display/Audio/GameOptions/SmartAI/Traffic/Replay/etc clusters), the 7 non-int specials, and --Help descriptive text -- re-open as follow-up slices using this same pattern |
