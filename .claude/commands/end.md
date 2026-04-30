# TD5RE End Session Workflow

End a `/fix` session: rebase against master, recheck the changes, surface any stubs/TODOs/uncertainties, then merge + tear down the worktree on user approval.

**Usage:** `/end` (run from the main tree or from inside any active `/fix` worktree)

---

## Step 0: Orient — detect context

Determine the **current session's** worktree and recover `SESSION_TAG` + `WORKTREE_DIR`. This skill always operates on the worktree belonging to *this conversation* — other active worktrees from parallel sessions are not this skill's concern.

**Check for second-invocation sentinel first:**

```bash
# If this file exists we are on the second /end call — treat as merge confirmation.
# (Created by Step 5 on the first /end call.)
ls C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending 2>/dev/null && echo "SECOND_INVOCATION=1" || echo "SECOND_INVOCATION=0"
```

If `SECOND_INVOCATION=1`: read `SESSION_TAG` + `WORKTREE_DIR` from `.end_pending`, delete the file, then **jump directly to Step 5 merge execution** (skip Steps 1–4).

**Otherwise (first invocation):** recover the current session's worktree:

1. If `SESSION_TAG` / `WORKTREE_DIR` are already in scope from a `/fix` session in this conversation, use those directly.
2. Otherwise infer from current branch:

```bash
SESSION_TAG="$(git rev-parse --abbrev-ref HEAD)"
WORKTREE_DIR="$(git rev-parse --show-toplevel)"
echo "Session: ${SESSION_TAG}"
echo "Worktree: ${WORKTREE_DIR}"
```

If there are **no active worktrees** and we're in the main tree with a clean branch, this skill still runs — it just skips the worktree teardown and treats the main tree as the unit of review.

---

## Step 1: Rebase

Bring the worktree branch up to date with master before rechecking. This catches any conflicts introduced by parallel `/fix` sessions that merged while this one was in progress.

```bash
cd "${WORKTREE_DIR}"

# Fetch latest master from origin so the rebase is against the true remote state.
git fetch origin master --quiet

# Rebase the current branch on origin/master.
REBASE_OUT="$(git rebase origin/master 2>&1)"
REBASE_EXIT=$?
echo "${REBASE_OUT}"
```

**If the rebase exits non-zero:**
- Do NOT auto-resolve. Show the conflicting files:
  ```bash
  git status --short
  ```
- Report to the user: which files conflict, what the conflict looks like (`git diff`). Ask them to resolve manually, then resume with `git rebase --continue`. Do NOT proceed to Step 2 until the rebase is clean.
- If the user decides to abort the rebase: `git rebase --abort`, then ask whether to end the session anyway (skip rebase, go straight to recheck) or stop entirely.

**If the rebase succeeds:** continue.

---

## Step 2: Recheck

Scan everything the worktree branch changed against master. The goal: surface stubs, open TODOs, and uncertainty markers that could represent regressions or deferred work before the branch merges.

### 2a. What changed?

```bash
cd "${WORKTREE_DIR}"

# All files changed by this branch (against master after rebase).
CHANGED_C="$(git diff --name-only origin/master...HEAD -- '*.c' '*.h')"
echo "Changed source files:"
echo "${CHANGED_C}"
```

If `CHANGED_C` is empty (branch only changed non-source files), skip to Step 2d.

### 2b. Stub + TODO scan

```bash
# Grep changed source files for deferred/incomplete markers.
# Run from worktree root so paths are relative and readable.
cd "${WORKTREE_DIR}"

for f in ${CHANGED_C}; do
    [ -f "${f}" ] || continue
    echo "=== ${f} ==="
    grep -n -E \
        'TODO|FIXME|STUB|stub|placeholder|Placeholder|return 0.*TODO|\/\*.*not.*impl|UNIMPLEMENTED|UNCERTAIN|\[UNCERTAIN\]|hardcoded 0|hardcoded.*TODO' \
        "${f}" || true
done
```

### 2c. Semgrep static scan (changed files only)

```bash
cd "${WORKTREE_DIR}"
if command -v semgrep >/dev/null 2>&1; then
    semgrep --config=auto --lang=c --quiet ${CHANGED_C} 2>&1 | head -80
else
    echo "(semgrep not available — skipping)"
fi
```

### 2d. Build verification (confirm rebase didn't break compile)

```bash
cd "${WORKTREE_DIR}/td5mod/src/td5re"
./build_standalone.bat $$ 2>&1 | tail -30
```

If the build fails after the rebase, fix compile errors (up to 2 attempts) before surfacing the recheck report.

### 2e. Recheck gate — STOP if findings exist

Collect everything from 2b–2d. If **any** of the following are present, **STOP and present to the user before proceeding to merge:**

1. Lines matching the stub/TODO grep above
2. Semgrep findings with severity ERROR or WARNING
3. A failed build

Present the findings in this format:

```
RECHECK FINDINGS — ${SESSION_TAG}
==================================

[STUBS / TODOs in changed files]
  <file>:<line>: <matching text>
  ...

[SEMGREP]
  <findings if any>

[BUILD]
  PASS / FAIL (with error excerpt if FAIL)

──────────────────────────────────
Options:
  1. Work on these now before merging  → stay in the worktree, fix items, then re-run /end
  2. Defer them all as TODOs           → /end will save them to memory and merge as-is
  3. Stop without merging              → leave the worktree open for later
```

**Wait for an explicit choice from the user.** Do not auto-choose. If the user picks option 1, stop here — let them work. They will re-invoke `/end` when ready. If they pick option 2, continue to Step 3 (save as TODOs). If they pick option 3, stop.

If **no findings**: report "Recheck clean — no stubs, TODOs, or build issues found." and proceed automatically to Step 3.

---

## Step 3: Save deferred items to memory

Run this step only when the user chose "Defer" in Step 2e, OR when the recheck was clean (nothing to defer, but we still want to log the session close).

### 3a. Build the TODO list from recheck findings

For each stub/TODO grep hit that the user chose to defer, compose a short memory entry. Group hits by file, deduplicate lines that say the same thing, and write one memory file per thematic cluster (not one per line).

**Memory file format:**

```markdown
---
name: TODO — <short description of the deferred item>
description: <one-line hook for MEMORY.md>
type: project
---

Deferred from session ${SESSION_TAG} ($(date +%Y-%m-%d)).

<file>:<line> — <stub/TODO text>
<file>:<line> — ...

**Why:** Left in place because <user's reason from option 2, or "no time this session">.
**How to apply:** Before implementing: Ghidra-verify the original function at <address if known>. Port verbatim per the byte-faithful rule.
```

Write to `C:/Users/maria/.claude-account3/projects/C--Users-maria-Desktop-Proyectos-TD5RE/memory/todo_<slug>.md` and add a one-line pointer to `MEMORY.md`.

Check `MEMORY.md` first — if a matching TODO entry already exists, update it rather than creating a duplicate.

### 3b. Mark resolved TODOs

Look at the changed files. Any TODO or stub that was present in the pre-session version (check `git show origin/master:<file> | grep TODO`) but is now gone means the session resolved it. If a matching memory entry exists in `MEMORY.md`, update it (mark RESOLVED, add date and commit hash) or remove it if there's nothing left to track.

```bash
cd "${WORKTREE_DIR}"
# For each changed file, diff the TODO markers between master and HEAD
for f in ${CHANGED_C}; do
    [ -f "${f}" ] || continue
    BEFORE="$(git show origin/master:${f} 2>/dev/null | grep -c -E 'TODO|STUB|placeholder' || echo 0)"
    AFTER="$(grep -c -E 'TODO|STUB|placeholder' "${f}" 2>/dev/null || echo 0)"
    if [ "${AFTER}" -lt "${BEFORE}" ]; then
        echo "${f}: ${BEFORE} → ${AFTER} stubs/TODOs (net ${$(( BEFORE - AFTER ))} resolved)"
    fi
done
```

---

## Step 4: Commit any remaining uncommitted changes

Before merging, make sure the worktree branch is fully committed. Any file the session edited but left unstaged (e.g. `td5re.ini` tweaks that were test-only, log files) should be handled:

```bash
cd "${WORKTREE_DIR}"
git status --short
```

- **Source files (`.c`, `.h`)**: stage and commit if they're changed and belong to the fix.
- **`td5re.ini`**: do NOT commit. This file is test scaffolding.
- **`log/`**: do NOT commit. Gitignored runtime output.
- **`build/` artifacts**: do NOT commit.

```bash
cd "${WORKTREE_DIR}"

# Stage source only — never td5re.ini, log/, build/
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h 2>/dev/null || true
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true
git add re/analysis/ re/sessions/ 2>/dev/null || true   # RE notes if any

# Same forbidden-path guard as /fix Step 3
BAD="$(git diff --cached --name-only | grep -E '^(original/|re/)' | grep -vE '^re/(analysis|sessions|assets/static/.*\.dat)$' || true)"
if [ -n "${BAD}" ]; then
    echo "REFUSING TO COMMIT — forbidden paths staged:"
    echo "${BAD}"
    echo "Unstage with: git reset HEAD -- <path>"
    exit 1
fi

# Only commit if there's something staged
if git diff --cached --quiet; then
    echo "Nothing staged — no additional commit needed."
else
    git commit -m "fix: <session work summary from $ARGUMENTS>

Session: ${SESSION_TAG}
Deferred TODOs: <list from Step 3a, or 'none'>

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
fi
```

---

## Step 5: Merge into master

**First invocation — present summary and pause:**

Print the merge summary, write the sentinel file with `SESSION_TAG` + `WORKTREE_DIR` recorded inside it, then **stop and wait for the user to re-invoke `/end`** to confirm:

```bash
printf "SESSION_TAG=%s\nWORKTREE_DIR=%s\n" "${SESSION_TAG}" "${WORKTREE_DIR}" \
    > C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending
```

```
MERGE SUMMARY — ${SESSION_TAG}
==============================
Branch commits:
  <git log --oneline origin/master..HEAD>

Files changed:
  <git diff --stat origin/master..HEAD>

Deferred TODOs saved to memory:
  <list from Step 3a, or 'none'>

Resolved TODOs:
  <list from Step 3b, or 'none'>

Re-run /end to confirm merge, or type "stop" to leave the worktree open.
```

Do NOT proceed further on the first invocation. Wait for the user.

**Second invocation (sentinel was present — merge confirmed):**

Delete the sentinel (already done in Step 0), then execute the merge:

```bash
rm -f C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending || true
```

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Sanity: main tree must be clean.
git status --porcelain
# If dirty: STOP and ask user to commit or stash main-tree changes first.

# Pre-merge forbidden-path guard (same as /fix Step 4)
BAD_DIFF="$(git diff --name-status master.."${SESSION_TAG}" \
    | awk '$2 ~ /^(original\/|re\/)/ && $2 !~ /^re\/(analysis|sessions|assets\/static\/.*\.dat)$/ {print}')"
if [ -n "${BAD_DIFF}" ]; then
    echo "REFUSING TO MERGE — branch touches forbidden paths:"
    echo "${BAD_DIFF}"
    exit 1
fi

# Pre-condition: original/ and re/assets/ must exist.
test -d original && test -d re/assets || {
    echo "PRECONDITION FAILED: main tree missing original/ or re/assets/. Do not merge."
    exit 1
}

git merge --no-ff "${SESSION_TAG}" -m "Merge ${SESSION_TAG}: <one-line session summary>"

# Post-merge guard
if [ ! -d original ] || [ ! -d re/assets ]; then
    echo "POST-MERGE FAILURE: original/ or re/assets/ disappeared. Reverting."
    git reset --hard ORIG_HEAD
    exit 1
fi

git push origin master
```

Verify push:
```bash
git fetch origin master --quiet
AHEAD="$(git rev-list --count origin/master..master)"
[ "${AHEAD}" -gt 0 ] && { echo "Push still pending — retry manually."; exit 1; }
echo "origin/master in sync."
```

---

## Step 6: Tear down the worktree (junction-safe)

Same protocol as `/fix` Step 4 — no exceptions.

```bash
# Pre-unlink the mingw junction with a hardcoded backslash path.
# Do NOT use $(cygpath -w ...) — it silently fails on this host.
cmd //c "rmdir \"C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\.claude\\worktrees\\${SESSION_TAG}\\td5mod\\deps\\mingw\""

# MANDATORY: verify junction is gone before running --force.
if [ -d "${WORKTREE_DIR}/td5mod/deps/mingw" ]; then
    echo "ERROR: pre-unlink failed — junction still live. Do NOT proceed."
    echo "Inspect: cmd //c 'dir /AL' inside ${WORKTREE_DIR}/td5mod/deps/"
    exit 1
fi

cd C:/Users/maria/Desktop/Proyectos/TD5RE
git worktree remove --force "${WORKTREE_DIR}"
git branch -d "${SESSION_TAG}"

# Stale-build janitor (2026-04-29). Sweep any orphan build_<pid>/ dirs
# from main's td5mod/src/td5re/. Numeric suffix only — named variants
# (e.g. build_ttfonts/) are preserved in case they're intentional.
shopt -s nullglob
for d in C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/build_[0-9]*/; do
    base=$(basename "$d")
    if [[ "$base" =~ ^build_[0-9]+$ ]]; then
        rm -rf "$d"
    fi
done
shopt -u nullglob
```

If `git branch -d` rejects (branch not fully merged): this should not happen since we just merged — report the error and do not use `-D`.

---

## Step 7: Propagate master into sibling worktrees

If other `fix-*` worktrees are still active, fast-forward them to the new master (same loop as `/fix` Step 4.5):

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
for WT in .claude/worktrees/fix-*; do
    [ -d "${WT}" ] || continue
    git -C "${WT}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || continue
    echo "=== Propagating master into ${WT} ==="
    if ! MERGE_ERR="$(git -C "${WT}" merge --no-ff --no-edit master 2>&1)"; then
        echo "${MERGE_ERR}"
        if echo "${MERGE_ERR}" | grep -qiE "permission denied|unable to (unlink|create|write)|text file busy"; then
            echo "STOPPED: ${WT} has locked files. Abort partial merge and retry manually."
            git -C "${WT}" merge --abort 2>/dev/null || true
            break
        fi
        if echo "${MERGE_ERR}" | grep -qi "conflict"; then
            echo "STOPPED: conflict in ${WT}. Resolve manually."
            break
        fi
        echo "STOPPED: unexpected failure in ${WT}."
        break
    fi
done
```

---

## Step 8: Session close report

Print a final summary to close the session cleanly:

```
SESSION CLOSED — ${SESSION_TAG}
================================
Merged: yes / skipped (reason)
Push:   origin/master in sync / pending (reason)

Resolved this session:
  <list from Step 3b or 'none'>

Deferred to memory:
  <list of new TODO memory entries from Step 3a, or 'none'>

Open worktrees remaining:
  <git worktree list output, filtered to fix-* only>

Next recommended action:
  <if deferred TODOs exist: "Run /fix <item> to address deferred stubs">
  <if sibling worktrees exist: "Review and merge or discard remaining fix-* worktrees">
  <if everything clean: "Nothing pending — session complete.">
```

---

## Key Rules

- **Never merge to master on the first `/end` invocation** — always pause at Step 5 and write the sentinel. The second `/end` invocation is the approval.
- **Never teardown the worktree without pre-unlinking the mingw junction** using the hardcoded backslash path — not `cygpath`.
- **Never auto-resolve rebase conflicts** — always stop and surface them.
- **Never commit `td5re.ini`, `log/`, or build artifacts**.
- **Never use `git branch -D`** during teardown — only `-d`. A worktree that wasn't merged should not be torn down by this skill; ask the user first.
- **If recheck surfaces uncertainties and the user chooses to work on them**: stop here, let the user work, and wait for `/end` to be re-invoked. Do NOT keep running the merge steps.
- **If `git worktree list` shows no active `fix-*` worktrees and we're in the main tree**: Steps 5–7 reduce to just a push + memory update. Skip teardown entirely.
