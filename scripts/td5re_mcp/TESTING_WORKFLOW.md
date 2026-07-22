# TD5RE testing workflow — live-control MCP + two-account delegation

How ongoing game testing runs across the two Claude accounts, using the
live-control surface (td5_control.c ↔ scripts/td5re_mcp) and the scenario
library (scenarios/). One-shot v1 bring-up notes live in
[TESTING_HANDOFF.md](TESTING_HANDOFF.md); this file is the durable loop.

## Roles

| | account2 "worker" (claude2, Accenture) | account3 "orchestrator" (claude3) |
|---|---|---|
| Can | Read/Grep/Glob, analysis, drafting text | everything: build, launch game, MCP, edits, git |
| Cannot | MCP servers, launching the game, writing files | (quota-limited — don't burn it on bulk reading) |
| Does | triage pending_to_test.csv, draft scenario scripts as text, analyze logs/reports/framedumps, write verdicts | build, run scenarios/MCP sessions, save worker drafts, flip CSV statuses, commit |

Worker invocation (from any orchestrator session):

```
pwsh -NoProfile -File "C:\Users\maria\Desktop\Proyectos\.claude\skills\repo-fleet\scripts\delegate.ps1" -Prompt "<task>" -Repo TD5RE
```

(Read-only Agent/Task spawns under Proyectos are auto-routed to the worker by
the agent-gate hook; `[local]` prefix forces local when execution is needed.)

## The test surface (three tiers)

1. **Selftest suite** (`pwsh scripts/selftest.ps1 [-Suite full]`) — regression
   net: screen walk, race matrix, golden traces, render goldens, degradation.
   Run before/after C changes. Headless, deterministic, no human judgement.
2. **Scenario library** (`python scripts/td5re_mcp/scenarios/run_all.py`) —
   live-control driven behaviors selftest can't reach: human-slot driving
   (cop chase, natural drag finish, crash damage), interactive UI nav,
   option persistence, arcade power-up acquisition. Each scenario prints
   per-check PASS/FAIL, exits 0/1; report at `log/scenario_report.md`,
   evidence PNGs at `log/scenarios/`.
3. **Interactive MCP sessions** — for exploratory testing / reproducing a
   reported bug: `launch_game()`, `start_race(...)`, `hold_action(...)`,
   `get_state()`, `screenshot_game()`, `read_log()`. Anything reproduced this
   way that's worth keeping becomes a new scenario file.

## The burn-down loop (pending_to_test.csv)

`td5mod/src/td5re/pending_to_test.csv` (`"summary","detail","status"`; rows
with status `pending`/blank are open). Loop, batch of 10–15 rows per cycle.

The standing full classification lives in [TRIAGE_LEDGER.md](TRIAGE_LEDGER.md)
(every open row → auto / auto-blocked / manual / covered-by-selftest, plus the
per-cycle batch order). Read it first each cycle and update the affected rows
in place rather than re-triaging the whole CSV from scratch.

1. **Triage (worker, read-only).** Classify each open row:
   - `auto` — assertable today via control verbs + get_state/racers[]/logs.
     Worker drafts the scenario script AS TEXT (it cannot write files),
     following the `scenarios/_lib.py` conventions (state predicates via
     `wait_until`, never wall-clock sleeps; `framedump` evidence; `check` +
     `finish`).
   - `auto-blocked:<missing verb/field>` — automatable once the control
     surface grows a named verb/state field. These feed the next control-
     surface increment.
   - `manual` — needs hardware (FFB/gamepad), audio judgement, real second
     machine, or visual look-dev. Stays for the user.
   - `covered-by-selftest` — already exercised; just needs the status flip.
2. **Run (orchestrator).** Review + save worker drafts into `scenarios/`,
   run them (`run_all.py <names>`), collect `log/scenario_report.md` +
   framedumps + log tails. Commit new scenarios that earn their keep.
3. **Analyze (worker, read-only).** Point the worker at the committed report
   + evidence paths; it writes per-row verdicts and failure hypotheses.
4. **Close (orchestrator).** For passes: flip the CSV row's `status` to
   `tested` (status column ONLY — the in-game DEL-overlay convention rewrites
   the same file). For failures: file via `/fix` (which logs changelog +
   pending per the standing routine). Commit with a path-scoped commit.

## Conventions

- **Evidence** lives under `log/scenarios/` (gitignored with the rest of
  log/); reference paths in write-ups rather than pasting image dumps.
- **Parallel safety**: scenarios/PID-scoped kills only (launcher.stop), never
  `/IM`. Concurrent game instances need distinct `TD5RE_CONTROL_PORT` values
  (both sides read it).
- **Golden invariance**: any change to td5_control.c / td5_input.c must
  re-run the full selftest; the control path must be a no-op when no client
  is connected.
- **Screenshots**: always `screenshot_game()` / the `framedump` verb
  (in-engine backbuffer). GDI `screenshot()` is a fallback only — black when
  occluded.
- **Ask before manual-drive launches**: launching the game for the USER to
  drive still requires asking first (standing feedback); scenario/selftest
  runs are automated and exempt.
