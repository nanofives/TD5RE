# TD5RE Account Sync

Sync conversations, memory, and relevant config from the current Claude account to another account directory.

## Source and destination

- **Source**: The current account directory (detect from `~/.claude-account*` based on which account is active — check `$CLAUDE_CONFIG_DIR` or infer from the `.credentials.json` that matches the running session).
- **Destination**: `C:\Users\maria\.claude-account1`

Both accounts share the same project directory: `C:\Users\maria\Desktop\Proyectos\TD5RE`

The project-scoped subdirectory inside each account is:
`projects/C--Users-maria-Desktop-Proyectos-TD5RE/`

## What to sync

Run these rsync/copy operations (use robocopy or cp -r since we're on Windows with bash):

### 1. Memory files (most important)
Sync all memory `.md` files from the source project memory dir to the destination project memory dir.
```
SOURCE: <source_account>/projects/C--Users-maria-Desktop-Proyectos-TD5RE/memory/
DEST:   <dest_account>/projects/C--Users-maria-Desktop-Proyectos-TD5RE/memory/
```
- Copy all `.md` files, overwriting older versions
- Do NOT delete files in dest that don't exist in source (account1 may have its own memories)

### 2. Session history (conversation logs)
Sync session `.jsonl` files and their companion directories from the source project dir to dest.
```
SOURCE: <source_account>/projects/C--Users-maria-Desktop-Proyectos-TD5RE/*.jsonl
SOURCE: <source_account>/projects/C--Users-maria-Desktop-Proyectos-TD5RE/*/  (session subdirs)
DEST:   <dest_account>/projects/C--Users-maria-Desktop-Proyectos-TD5RE/
```
- Only copy sessions that don't already exist in dest (skip if `.jsonl` already present)
- This avoids re-copying large files needlessly

### 3. Global history
```
SOURCE: <source_account>/history.jsonl
DEST:   <dest_account>/history.jsonl
```
- Append new entries from source that aren't in dest (or just overwrite if source is newer)

### 4. Plans and tasks
```
SOURCE: <source_account>/plans/
DEST:   <dest_account>/plans/
SOURCE: <source_account>/tasks/
DEST:   <dest_account>/tasks/
```

## Execution

1. First, identify the source account dir. The current session is running from one of `~/.claude-account1`, `~/.claude-account2`, or `~/.claude-account3`. Check which one by looking at the environment or reading `.credentials.json` files.

2. Create any missing destination directories with `mkdir -p`.

3. Run the copies using `cp` commands in bash. Show a summary of what was synced:
   - Number of memory files copied/updated
   - Number of sessions copied
   - Whether history was updated

4. Report the total size transferred.

## Important
- Never overwrite `.credentials.json` or `settings.json` — those are account-specific
- Never delete anything in the destination
- If the destination project dir doesn't exist, create it
