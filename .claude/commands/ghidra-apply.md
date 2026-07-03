# Ghidra Apply

Apply rename + comment proposals from one or more batch files to the master Ghidra project (`TD5.gpr`). Updates master, appends to header, then regenerates the offline export `re/ghidra_export/`. Idempotent — running twice on the same batch is a no-op for already-applied renames.

**Usage:** `/ghidra-apply <batch-path> [<batch-path> ...]`

Examples:
- `/ghidra-apply re/analysis/global_naming/batch_26_some_area.md`
- `/ghidra-apply re/analysis/function_naming/batch_01_audio.md`
- `/ghidra-apply re/analysis/global_naming/batch_26_*.md`

Batch file format: see `re/analysis/global_naming/BATCH_TEMPLATE.md`. Supports two optional tables — **`## Proposals (globals)`** and **`## Proposals (functions)`**. Either or both can be present.

## When to delegate

- **≤2 batches AND ≤40 total proposals**: do the work in-thread.
- **Otherwise**: spawn a general-purpose agent (the work runs ~1 sec/proposal; large applies blow up main context). Use the agent prompt template at the bottom of this skill.

## Workflow

### Phase 0 — Safety

1. **Validate batches**: confirm each `<batch-path>` exists and has at least one `## Proposals` section. Skip silently and warn the user if a path is missing.
2. **Backup master**:
   ```bash
   BACKUP="TD5.rep.bak.$(date +%Y%m%d-%H%M%S)-pre-apply"
   cp -r TD5.rep "$BACKUP"
   echo "Backup: $BACKUP"
   ```
3. **Check master is free**: confirm no `TD5.lock` exists at the repo root (`ls TD5.lock* 2>/dev/null` — empty means available). If a lock exists and the user confirms no Ghidra session is running, it's stale: `rm -f TD5.lock TD5.lock~`.

### Phase 1 — Load Ghidra MCP schemas

Use ToolSearch to load:
```
ToolSearch(query="select:mcp__ghidra__project_program_open_existing,mcp__ghidra__program_save,mcp__ghidra__program_close,mcp__ghidra__transaction_begin,mcp__ghidra__transaction_commit,mcp__ghidra__symbol_rename,mcp__ghidra__function_rename,mcp__ghidra__comment_set,mcp__ghidra__health_ping", max_results=9)
```

### Phase 2 — Open master writable

This is the **EXCEPTION to the read-only HARD RULE** — master IS the writable target (single-writer: never run two applies concurrently).

```
project_program_open_existing(
  project_location="C:/Users/maria/Desktop/Proyectos/TD5RE",
  project_name="TD5",
  program_name="TD5_d3d.exe",
  read_only=false
)
```

The tool returns a `session_id` — use it for all subsequent calls.

If open fails with "already locked", abort — another session has master open. Do not retry without user confirmation.

### Phase 3 — Apply each batch

For each batch file:

1. Read the file. Find both proposal tables (one or both may be absent):
   - `## Proposals (globals)` — columns: address, size, proposed_name, confidence, evidence, port_mirror
   - `## Proposals (functions)` — columns: address, current_name, proposed_name, confidence, evidence, port_mirror
2. `transaction_begin(description="ghidra-apply <batch_basename>")`
3. For each global proposal row:
   - Parse `address` (e.g. `0x004afb84`), `proposed_name`, `confidence`, `evidence`.
   - **high**: `symbol_rename(session_id, address, new_name=proposed_name)`
   - **medium**: `symbol_rename(session_id, address, new_name=proposed_name + "_PROVISIONAL")` (skip suffix if already in name OR if batch frontmatter has `area: conflict_triage` — triage batches use names literally)
   - **low**: skip rename
   - All confidence levels: `comment_set(session_id, address, comment_type="EOL", comment=evidence + " [<batch_basename> <today>]")`
   - **Skip with warning** if rename errors:
     - `"no symbol found at <addr>"` → operand-encoding artifact (offset inside an existing array). Skip rename, still add comment.
     - `"name already used"` with same name → idempotent no-op, fine.
     - `"name already used"` with different name → conflict log; comment preserves both interpretations.
4. For each function proposal row:
   - Parse `address`, `proposed_name`, `confidence`, `evidence`.
   - **high**: `function_rename(session_id, function_start=address, name=proposed_name)`
   - **medium**: `function_rename(session_id, function_start=address, name=proposed_name + "_PROVISIONAL")`
   - **low**: skip rename
   - All: `comment_set(session_id, address, comment_type="PLATE", comment=evidence + " [<batch_basename> <today>]")`
5. `transaction_commit`
6. `program_save`
7. Append a status line to `re/analysis/global_naming/CONSOLIDATION_LOG.md`:
   ```
   ## <batch_basename> applied <YYYY-MM-DD>: G renames + F function-renames + C comments + S skipped
   ```

**Hard rules**:
- Save after every batch — survive crashes.
- One transaction per batch.
- Do not retry failed renames more than once.
- Do not attempt struct promotions or type changes — those need a different skill.

### Phase 4 — Append to header

For each high-confidence GLOBAL successfully renamed, append to `td5mod/src/td5re/td5_orig_globals.h` under a fresh section:

```c
/* ---- Added <YYYY-MM-DD> from <batch_basename(s)> ---- */
#define DAT_<addr_lower>  <name> /* <batch> */
...
```

Sort within the section by address. Skip if no new high-confidence global renames applied. Functions don't go in the header (different consumer).

### Phase 5 — Close + republish

1. `program_close` (releases master lock)
2. Regenerate the offline export so other sessions see the new annotations (the pool of clones was retired 2026-07-03; `re/ghidra_export/` is what sessions read now):
   ```bash
   cd C:/Users/maria/Desktop/Proyectos/TD5RE
   ghidra_12.0.3_PUBLIC/support/analyzeHeadless.bat . TD5 -process TD5_d3d.exe \
     -noanalysis -readOnly -scriptPath scripts -postScript ExportAllDecomp.java
   ```
3. Confirm `re/ghidra_export/EXPORT_INFO.txt` has fresh counts and 0 decompile failures.

### Phase 6 — Report

Concise summary:
- Per batch: G global renames + F function renames + C comments + S skipped
- Header lines appended: N
- Offline export regenerated: yes/no (+ function count from EXPORT_INFO.txt)
- Any conflicts (name already taken with different name): list with both names

## Agent delegation template (for large applies)

When ≥3 batches OR ≥40 proposals, spawn a `general-purpose` agent with this prompt:

```
You are the /ghidra-apply executor. Apply the following batch file(s) to the master Ghidra project:
<list batch paths>

Working dir: C:\Users\maria\Desktop\Proyectos\TD5RE

Read .claude/commands/ghidra-apply.md for the full procedure. Follow phases 1-6.

IMPORTANT discipline:
- Open master with read_only=false (EXCEPTION to HARD RULE)
- One transaction + save per batch (crash safety)
- Hard time-box: if >30 tool calls on a single batch without progress, log + skip
- Do NOT attempt struct promotions or type changes
- Do NOT re-read batch files; parse the proposals table once and apply

Return a concise summary (≤300 words) of results.
```

Run the agent in the foreground (master is single-writer; you can't parallelize). Wait for completion before reporting.

## Notes

- This skill uses the same backup convention as the original T1-T5 consolidation: `TD5.rep.bak.<timestamp>-pre-apply/`. Keep these around for a few weeks in case a rename needs to be reverted.
- If a rename produces a conflict ("name already used") that's NOT the same name being re-applied, log both names in the CONSOLIDATION_LOG and let the user triage manually. Do not auto-resolve.
- Function renames belong in `re/analysis/function_naming/` (parallel directory to `global_naming/`) once that workflow exists. Until then, function-only batches live alongside the globals.
- The skill does NOT remove old `#define`s from the header. If a rename overwrites an existing rename (existing-name correction), the old `#define` lingers as a duplicate. Either edit by hand or regenerate the full header from Ghidra state in a separate pass.
