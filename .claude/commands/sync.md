# Account Sync (current project)

Sync the current project's memory and session history between two Claude account directories. The project is determined by the working directory at the time the skill runs — whichever folder Claude Code was started in.

## Usage

```
/sync <source> <dest>
```

`<source>` and `<dest>` are account numbers: `1`, `2`, or `3` (mapping to `~/.claude-account1`, `~/.claude-account2`, `~/.claude-account3`).

Examples:
- `/sync 3 2` — pull from account3 into account2
- `/sync 2 1` — push from account2 to account1
- `/sync 1 3` — copy account1's project state into account3

If the user gives ambiguous input (e.g. "from claude3"), ask which destination they mean.

## Project resolution (dynamic)

Derive the per-account project subdirectory name from `$PWD` at runtime:

```bash
# On Windows + Git Bash:
PROJECT_DIR=$(cygpath -w "$PWD" | sed 's|[:\\/]|-|g')
# Example: C:\Users\maria\Desktop\Proyectos\TD5RE → C--Users-maria-Desktop-Proyectos-TD5RE
```

This matches Claude Code's own encoding (colons, backslashes, and forward slashes all become `-`).

The full per-account project path is then:
```
~/.claude-account<N>/projects/$PROJECT_DIR/
```

Echo the resolved `$PROJECT_DIR` to the user before copying so they can confirm it's the project they expect.

## Scope — PROJECT ONLY

This sync **only touches files inside the current project's subdirectory** of each account:

```
~/.claude-accountN/projects/$PROJECT_DIR/
```

Anything outside that subdirectory (other projects, account-level config, etc.) is **never read or modified**.

Specifically, the sync **does NOT touch**:
- `history.jsonl` (global shell history — cross-project)
- `plans/` (global, no project tagging)
- `tasks/` (global, indexed by UUID, no project tagging)
- `settings.json`, `.credentials.json` (account-specific)
- `cache/`, `telemetry/`, `plugins/`, `backups/`, etc.
- Any other `projects/<other-project>/` subdirectory

If the user explicitly asks to also sync global history/plans/tasks, warn them that this affects all projects on the destination account, then ask for confirmation before proceeding.

## What gets synced (project-scoped only)

### 1. Memory files
```
SOURCE: ~/.claude-account<source>/projects/$PROJECT_DIR/memory/*.md
DEST:   ~/.claude-account<dest>/projects/$PROJECT_DIR/memory/
```
- Copy all `.md` files, **overwriting** files with the same name in dest.
- **Never delete** files in dest that don't exist in source (dest may have its own memories).

### 2. Session history (conversation logs)
```
SOURCE: ~/.claude-account<source>/projects/$PROJECT_DIR/*.jsonl
SOURCE: ~/.claude-account<source>/projects/$PROJECT_DIR/*/  (session subdirs)
DEST:   ~/.claude-account<dest>/projects/$PROJECT_DIR/
```
- **Append-only**: skip files already present in dest.
- Avoids re-copying large session logs.
- **Preserve timestamps**: copies must keep the source file's mtime/atime, because Claude Code's conversation picker uses the file mtime as the "last message date". Without `-p`, every synced conversation collapses to today's date in the destination account.

## Execution

1. Parse `<source>` and `<dest>` from the user's args. Validate both are `1`, `2`, or `3` and not the same.

2. Resolve the project dir name dynamically:
   ```bash
   PROJECT_DIR=$(cygpath -w "$PWD" | sed 's|[:\\/]|-|g')
   ```
   If `cygpath` is unavailable (non-Windows), fall back to:
   ```bash
   PROJECT_DIR=$(echo "$PWD" | sed 's|[:\\/]|-|g')
   ```

3. Resolve full paths:
   ```
   SRC=~/.claude-account<source>/projects/$PROJECT_DIR
   DST=~/.claude-account<dest>/projects/$PROJECT_DIR
   ```

4. Verify `SRC` exists. If not, abort with a clear error showing `$PROJECT_DIR` (so the user can sanity-check the resolution).

5. Show a **dry-run summary** before copying. Echo:
   - The resolved `$PROJECT_DIR` ("syncing project: `<name>`")
   - Source and dest account numbers
   - How many memory `.md` files would be overwritten in dest (with names)
   - How many memory `.md` files are new (not in dest)
   - How many session `.jsonl` files would be copied (skipping existing)
   - Ask the user to confirm before proceeding (unless they pre-confirmed).

6. Run the copies. **Always pass `-p` to preserve timestamps** — otherwise the destination account's conversation picker shows every synced conversation with today's date:
   - `mkdir -p` any missing dest subdirectories.
   - `cp -fp` for memory files (overwrite, preserve mtime).
   - `cp -np` for session files (no-clobber, preserve mtime). For recursive session subdirs, use `cp -rnp`.
   - Verify after copy: a sampled `stat -c '%y' <dst-file>` should equal the source file's mtime. If it doesn't, the user is on a `cp` build without `-p` support — fall back to `rsync -t` or, as a last resort, a Python loop using `shutil.copy2` (which preserves mtime by default).

7. Report:
   - Project: `$PROJECT_DIR`
   - Memory files: N overwritten, M added new
   - Sessions: K copied, L skipped (already existed)
   - Total bytes transferred

## Safety rules (hard rules)

- **Never** read or write any path outside `~/.claude-account<source|dest>/projects/$PROJECT_DIR/` by default.
- **Never** overwrite `.credentials.json`, `settings.json`, or any account-root file.
- **Never** delete anything in the destination.
- **Never** sync `history.jsonl`, `plans/`, or `tasks/` without explicit user opt-in and a warning that these are cross-project.
- **Never** touch any other `projects/<other-project>/` subdirectory — only the current `$PROJECT_DIR`.
- If `<source>` and `<dest>` resolve to the same path, abort.
- If `$PROJECT_DIR` resolves to something empty or suspicious (e.g. just `-` or `--`), abort and show the raw `$PWD` for debugging.
