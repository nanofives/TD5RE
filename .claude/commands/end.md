# TD5RE End Session Workflow

End a `/fix` session: rebase against master, recheck the changes, surface any stubs/TODOs/uncertainties, then merge + tear down the worktree on user approval.

**Usage:** `/end` (run from the main tree or from inside any active `/fix` worktree)

---

## Step 0: Orient — detect context

Determine the **current session's** worktree and recover `SESSION_TAG` + `WORKTREE_DIR`. This skill always operates on the worktree belonging to *this conversation* — other active worktrees from parallel sessions are not this skill's concern.

**Recover `SESSION_TAG` + `WORKTREE_DIR` FIRST** — the second-invocation sentinel is keyed on `SESSION_TAG`, so you must know the tag before you can find this session's own sentinel:

1. If `SESSION_TAG` / `WORKTREE_DIR` are already in scope from a `/fix` session in this conversation, use those directly. **This is the authoritative source** — prefer it over anything on disk.
2. Otherwise infer from the current branch (valid only when `/end` is run from inside the worktree):

```bash
SESSION_TAG="$(git rev-parse --abbrev-ref HEAD)"
WORKTREE_DIR="$(git rev-parse --show-toplevel)"
echo "Session: ${SESSION_TAG}"
echo "Worktree: ${WORKTREE_DIR}"
```

If `SESSION_TAG` resolves to `master` (run from the main tree with nothing in scope) and there are **no active worktrees**, this skill still runs — it just skips the worktree teardown and treats the main tree as the unit of review.

**Then check for THIS session's second-invocation sentinel:**

```bash
# Per-session sentinel — keyed on SESSION_TAG so concurrent /end runs in parallel
# sessions never clobber each other. The OLD design used a single shared
# `.end_pending` file: with multiple live worktrees it raced — one session's first
# /end overwrote another's, and a parallel second /end could delete it or even read
# the wrong SESSION_TAG and merge the wrong branch. Created by Step 5 on first /end.
SENTINEL="C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending.${SESSION_TAG}"
ls "${SENTINEL}" 2>/dev/null && echo "SECOND_INVOCATION=1" || echo "SECOND_INVOCATION=0"
```

**If `SECOND_INVOCATION=1`:** read `SESSION_TAG` + `WORKTREE_DIR` from `${SENTINEL}` and **sanity-check they match what you recovered above**. If the file's tag differs from the conversation-scope tag, trust the conversation scope and STOP to investigate — never merge a tag you did not recover yourself. Then delete the sentinel and **jump directly to Step 5 merge execution** (skip Steps 1–4).

**If `SECOND_INVOCATION=0` but the user explicitly re-invoked `/end`** to confirm a merge you already summarized in this conversation (the sentinel can go missing if a concurrent session's cleanup swept the worktrees dir, or an older shared `.end_pending` was removed): treat the explicit re-invocation as the approval — but **ONLY ever merge/teardown the `SESSION_TAG` you recovered from THIS conversation's `/fix` state.** Never merge a branch you cannot tie to this conversation.

**Otherwise (genuine first invocation):** proceed to Step 1 with the recovered `SESSION_TAG` / `WORKTREE_DIR`.

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

### 2d. Build verification (confirm rebase didn't break EITHER variant)

Build BOTH source-port executables — the dev binary (`td5re.exe`, full RE
instrumentation) and the release binary (`td5re_release.exe`, instrumentation
stripped). Building both here catches release-only regressions: a new ungated
pilot-trace call site that needs a `#ifndef TD5RE_RELEASE` guard, a broken
`TD5RE_RELEASE` `#ifdef`, or a failed instrumentation-strip check.

```bash
cd "${WORKTREE_DIR}/td5mod/src/td5re"
./build_all.bat 2>&1 | tail -50
```

If either build fails after the rebase, fix the compile/link errors (up to 2
attempts) before surfacing the recheck report. A release-link `undefined
reference to td5_pilot_trace_*` almost always means a newly-added ungated trace
call site needs a `#ifndef TD5RE_RELEASE` guard (the dev build won't catch it).

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

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
fi
```

---

## Step 5: Merge into master

**First invocation — present summary and pause:**

Print the merge summary, write the **per-session** sentinel file (named `.end_pending.${SESSION_TAG}` so parallel sessions never collide) with `SESSION_TAG` + `WORKTREE_DIR` recorded inside it, then **stop and wait for the user to re-invoke `/end`** to confirm:

```bash
printf "SESSION_TAG=%s\nWORKTREE_DIR=%s\n" "${SESSION_TAG}" "${WORKTREE_DIR}" \
    > "C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending.${SESSION_TAG}"
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

Delete this session's sentinel, then execute the merge:

```bash
rm -f "C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.end_pending.${SESSION_TAG}" || true
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

Same protocol as `/fix` Step 4 — no exceptions. See the **TEARDOWN SAFEGUARDS**
block in `fix.md` ("Abandoning a worktree") for the full rationale: main's
`original/` + `td5mod/deps/mingw/` now carry a deny-delete ACL, and there is an
off-disk backup at `C:\Users\maria\Desktop\TD5RE_backup_2026-05-28\`.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# CANARY (pre): irreplaceable main-tree markers must exist before we start.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "PRECONDITION: main-tree markers already missing — investigate first"; exit 1; }

# Pre-unlink the mingw junction. THIS HOST: cmd //c "rmdir" fails with a
# volume-label syntax error (confirmed 2026-05-28), leaving the junction live
# for --force to follow. Use PowerShell's reparse-aware delete (link only, never
# the target), guarded on ReparsePoint.
powershell.exe -NoProfile -Command "\$p='${WORKTREE_DIR}/td5mod/deps/mingw' -replace '/','\\'; if (Test-Path \$p){ \$i=Get-Item \$p -Force; if (\$i.Attributes -band [IO.FileAttributes]::ReparsePoint){ \$i.Delete() } else { Write-Error 'NOT a reparse point — refusing'; exit 1 } }"

# MANDATORY: verify junction is gone before running --force.
if [ -d "${WORKTREE_DIR}/td5mod/deps/mingw" ]; then
    echo "ERROR: pre-unlink failed — junction still live. Do NOT proceed."
    exit 1
fi

git worktree remove --force "${WORKTREE_DIR}"
git branch -d "${SESSION_TAG}"

# CANARY (post): the ACL should block any junction-follow, but verify the
# markers survived. If gone, restore from the backup IMMEDIATELY.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "FATAL: main-tree data vanished during teardown — restore from C:\\Users\\maria\\Desktop\\TD5RE_backup_2026-05-28\\ NOW"; exit 1; }

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

# Screenshot / scratch sweep (2026-05-30). /fix and /end visual-verification
# captures land in main's re/screenshots/ (untracked, gitignored scratch). They
# are throwaway evidence and the user does not want them kept — sweep so they
# don't accumulate. ONLY re/screenshots/ — never re/assets, re/analysis,
# re/sessions, or any tracked/important path.
if [ -d C:/Users/maria/Desktop/Proyectos/TD5RE/re/screenshots ]; then
    rm -rf C:/Users/maria/Desktop/Proyectos/TD5RE/re/screenshots
    echo "swept re/screenshots/ (throwaway verification captures)"
fi
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

## Step 7.5: Global house-cleaning — sweep stray branches + fully-merged worktrees

Steps 5–6 close *this* session's worktree, but `/fix` runs and subagent
worktree-isolation runs leave two kinds of debris that nothing else collects:

- **Stray branch refs** — `worktree-agent-*` branches (subagent worktrees whose
  directory the harness auto-removed but whose branch ref survived) and `fix-*`
  branches whose worktree is already gone. These are invisible to the `/fix`
  worktree janitor because it only walks `git worktree list`.
- **Fully-merged sibling worktrees** — old `/fix` worktrees whose branch is now
  entirely contained in master.

This step deletes only what is **provably disposable** (merged into master) and
**reports** anything with unmerged commits or uncommitted changes for your
decision — it NEVER force-merges. This is what makes `/end` actually "clean
house" instead of letting agent branches and old fix worktrees pile up.

### 7.5a — Sweep stray branch refs (no live worktree)

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
git fetch origin master --quiet 2>/dev/null || true

REPORT="$(mktemp)"   # accumulates unmerged leftovers for 7.5c

# Branches still checked out in a worktree are handled in 7.5b — exclude them.
CHECKED_OUT="$(git worktree list --porcelain | awk '/^branch /{sub(/^refs\/heads\//,"",$2);print $2}')"

for B in $(git for-each-ref --format='%(refname:short)' \
            'refs/heads/worktree-agent-*' 'refs/heads/fix-*'); do
    [ "${B}" = "master" ] && continue
    printf '%s\n' "${CHECKED_OUT}" | grep -qx "${B}" && continue   # has a worktree → 7.5b
    if git merge-base --is-ancestor "${B}" master 2>/dev/null; then
        # Fully in master — only the label remains. -d is safe (and refuses if
        # we somehow misjudged: that refusal is the backstop).
        git branch -d "${B}" >/dev/null 2>&1 && echo "deleted merged branch: ${B}"
    else
        AHEAD="$(git rev-list --count "master..${B}" 2>/dev/null || echo '?')"
        echo "BRANCH   ${B} — ${AHEAD} commit(s) not in master" >> "${REPORT}"
    fi
done

# Orphaned per-session sentinels — `.end_pending.<tag>` left by a session that ran
# its first /end but never confirmed (no second /end), plus any legacy bare
# `.end_pending` from the old shared-file design. Safe to remove once the matching
# worktree is gone; NEVER touch a sentinel whose worktree still exists (that
# session may still be about to confirm its merge).
for S in .claude/worktrees/.end_pending .claude/worktrees/.end_pending.*; do
    [ -e "${S}" ] || continue
    if [ "${S}" = ".claude/worktrees/.end_pending" ]; then
        rm -f "${S}" && echo "removed legacy shared sentinel: ${S}"
        continue
    fi
    TAG="${S#.claude/worktrees/.end_pending.}"
    if [ ! -d ".claude/worktrees/${TAG}" ]; then
        rm -f "${S}" && echo "removed orphaned sentinel: ${S} (worktree gone)"
    fi
done
```

### 7.5b — Close fully-merged sibling worktrees

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

SAFE="$(mktemp)"   # worktrees safe to auto-close (merged + clean)
git worktree list --porcelain | awk '
    /^worktree /{wt=$2}
    /^branch /  {sub(/^refs\/heads\//,"",$2); br=$2}
    /^$/        {if(wt)printf "%s\t%s\n",wt,br; wt="";br=""}
    END         {if(wt)printf "%s\t%s\n",wt,br}
' | while IFS=$'\t' read -r WT BR; do
    case "${WT}" in */.claude/worktrees/fix-*) ;; *) continue ;; esac   # never the main tree
    DIRTY="$(git -C "${WT}" status --porcelain 2>/dev/null | wc -l)"
    if [ -n "${BR}" ] && git merge-base --is-ancestor "${BR}" master 2>/dev/null && [ "${DIRTY}" -eq 0 ]; then
        printf '%s\t%s\n' "${WT}" "${BR}" >> "${SAFE}"
    else
        WHY=""; [ "${DIRTY}" -gt 0 ] && WHY="dirty=${DIRTY} "
        git merge-base --is-ancestor "${BR}" master 2>/dev/null || WHY="${WHY}unmerged"
        echo "WORKTREE ${WT} (${BR}) — ${WHY}" >> "${REPORT}"
    fi
done
```

Then tear down each entry in `${SAFE}` using the **Step 6 junction-safe teardown
block, verbatim** — plug `WORKTREE_DIR=<WT>` and `SESSION_TAG=<BR>` and run the
full block (pre/post canary, PowerShell reparse-delete of the `mingw` junction,
verify the junction is gone, then `git worktree remove --force`, then
`git branch -d`). Do NOT improvise a shorter teardown and never `rm -rf` a
worktree path. After the loop:

```bash
git worktree prune --verbose
```

### 7.5c — Report leftovers (these are your decision, never auto-touched)

```bash
if [ -s "${REPORT}" ]; then
    echo ""
    echo "UNMERGED LEFTOVERS — left untouched, need your decision:"
    sed 's/^/  /' "${REPORT}"
    echo "  → resume with /fix, or once you're sure it's disposable delete manually"
    echo "    (git branch -D <name> for a ref; Step 6 junction-safe teardown for a worktree)."
fi
rm -f "${REPORT}" "${SAFE}" 2>/dev/null || true
```

**Rules for this step:**
- **Only `git branch -d`, never `-D`.** `-d` refuses any branch not fully in master — that refusal IS the safety net. Unmerged branches are reported, never deleted.
- **Auto-close only MERGED + CLEAN worktrees.** Any uncommitted change or unmerged commit → report and leave alone, even if the name matches `fix-*` (it may be a candidate fix you're iterating on, e.g. an unfinished OOB rework).
- **Reuse the Step 6 teardown verbatim** for every removal — same junction pre-unlink + canary. Never `rm -rf`.
- **Idempotent** — re-running on a clean repo reports nothing and removes nothing.

---

## Step 7.6: Final commit & publish gate (guaranteed origin sync)

`/end` must leave the repo fully committed and pushed — you should never have to
manually `git add` / `commit` / `push` after a clean `/end`. Step 5 already merges
and pushes the *worktree branch*, but two things still slip through:

- **Main-tree-only work** done outside any worktree (skill edits in
  `.claude/commands/`, `.gitignore`, RE docs under `re/analysis/`, tooling under
  `scripts/`) — never part of a `/fix` branch, so Steps 4–5 don't touch it.
- **A guaranteed final push** after all cleanup, so every commit that exists —
  the merge, plus any main-tree commit below — lands on `origin/master`.

This step runs on the merge-confirmed (second) `/end` invocation **and** on a
main-tree-only `/end` (no worktree). It commits remaining **safe** main-tree
changes, then pushes as the last git action.

### 7.6a — Stage safe main-tree changes (explicit allowlist ONLY)

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Stage by explicit path. NEVER `git add -A` / `git add .` here — that is exactly
# how a runtime save (_cleanup_*/Config.td5) got committed on 2026-05-29.
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h 2>/dev/null || true
git add td5mod/src/td5re/*.bat 2>/dev/null || true        # build scripts (dev + release)
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true
git add td5mod/ddraw_wrapper/src/*.c td5mod/ddraw_wrapper/src/*.h 2>/dev/null || true
git add .claude/commands/*.md 2>/dev/null || true        # skill edits
git add re/analysis/ re/sessions/ 2>/dev/null || true     # RE notes
git add scripts/ 2>/dev/null || true                      # tooling
git add td5re_release.ini 2>/dev/null || true             # release config (NOT td5re.ini — that's test scaffolding)
git add .gitignore CLAUDE.md AGENTS.md 2>/dev/null || true

# Forbidden-path guard (same as Step 5): never original/ or re/ outside the
# tracked-notes whitelist.
BAD="$(git diff --cached --name-only | grep -E '^(original/|re/)' \
        | grep -vE '^re/(analysis|sessions|assets/static/.*\.dat)$' || true)"
if [ -n "${BAD}" ]; then
    echo "REFUSING TO COMMIT — forbidden paths staged:"; echo "${BAD}"
    echo "Unstage with: git reset HEAD -- <path>"; exit 1
fi
```

### 7.6b — Surface what was NOT staged (never blind-commit unknowns)

```bash
# Anything still unstaged or untracked after the allowlist is left for the user
# on purpose: td5re.ini (test scaffolding), *.td5 saves, loose scratch, or any
# file type the allowlist doesn't recognize. Do NOT `git add` these.
LEFT="$(git status --short | grep -vE '^[MARD]  ' || true)"
if [ -n "${LEFT}" ]; then
    echo "Left in working tree (NOT auto-committed — review/commit manually if wanted):"
    echo "${LEFT}" | sed 's/^/  /'
fi
```

### 7.6c — Commit (if anything staged) + guaranteed push

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

if git diff --cached --quiet; then
    echo "No main-tree changes to commit."
else
    git commit -m "chore(session): commit remaining main-tree work — ${SESSION_TAG}

<one-line summary: skill edits / RE docs / tooling committed this session>

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
fi

# Guaranteed final push — the LAST git action of /end. Captures the Step 5 merge,
# the Step 6/7.5 cleanup, and any main-tree commit above.
git push origin master
git fetch origin master --quiet
AHEAD="$(git rev-list --count origin/master..master)"
if [ "${AHEAD}" -gt 0 ]; then
    echo "POST-CONDITION FAILED: master is ${AHEAD} ahead of origin after push."
    echo "Retry 'git push origin master' or surface the auth/network error —"
    echo "do NOT report the session as shipped while commits are unpushed."
    exit 1
fi
echo "origin/master in sync — session fully published."
```

**Rules for this step:**
- **Never `git add -A` / `git add .`** — stage only the explicit allowlist. `td5re.ini`, `*.td5` saves, scratch, and unrecognized files are *listed* for the user, never auto-committed. (Direct lesson of the 2026-05-29 `Config.td5` mis-commit.)
- **The push is the final action.** `/end` is not done until `git rev-list --count origin/master..master` returns `0`. If the push fails, surface it — do not end as if shipped.
- **Idempotent** — if nothing is staged and master is already in sync, this step prints two status lines and changes nothing.

---

## Step 7.7: Rebuild both binaries into the project root

After the merge lands on master, rebuild BOTH source-port executables from the
**main tree** so the project-root binaries reflect the merged result. Step 2d
built inside the worktree (now torn down), and its binaries deployed to the
*worktree* root — not main. This step refreshes the real deliverables in the
project root.

Runs only on the merge-confirmed **second** `/end` invocation and on a
main-tree-only `/end`. (The first invocation stops at Step 5, so it never reaches
here — no half-built binaries from an unconfirmed merge.)

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re
./build_all.bat 2>&1 | tail -50
```

This deploys `td5re.exe` (dev) and `td5re_release.exe` (release) to the project
root. Both `.exe`s and the `build/` + `build_release/` object dirs are gitignored
(`*.exe`, `build_*/`) — never commit them.

**Rules for this step:**
- **Post-merge artifact refresh, not a merge gate.** The merge already landed and
  pushed in Step 5; if a build fails here, surface it in the Step 8 report but do
  NOT attempt to revert the merge — the source is fine, the local binary is just
  stale.
- **Always builds BOTH variants** via `build_all.bat`. A green dev build with a
  broken release build (or vice-versa) is still a failure to report.
- **Idempotent** — re-running rebuilds both binaries from the same merged master.

---

## Step 8: Session close report

Print a final summary to close the session cleanly:

```
SESSION CLOSED — ${SESSION_TAG}
================================
Merged: yes / skipped (reason)
Push:   origin/master in sync / pending (reason)
Binaries: td5re.exe + td5re_release.exe rebuilt into project root / build FAILED (reason)

Resolved this session:
  <list from Step 3b or 'none'>

Deferred to memory:
  <list of new TODO memory entries from Step 3a, or 'none'>

Cleaned up this session (Step 7.5):
  <count of stray branches deleted + fully-merged worktrees auto-closed, or 'none'>

Unmerged leftovers (left for your decision):
  <the Step 7.5c REPORT list, or 'none'>

Open worktrees remaining:
  <git worktree list output, filtered to fix-* only>

Next recommended action:
  <if deferred TODOs exist: "Run /fix <item> to address deferred stubs">
  <if unmerged leftovers exist: "Resume them with /fix, or delete once confirmed disposable">
  <if everything clean: "Nothing pending — session complete.">
```

---

## Key Rules

- **Never merge to master on the first `/end` invocation** — always pause at Step 5 and write the **per-session** sentinel (`.end_pending.${SESSION_TAG}`). The second `/end` invocation is the approval.
- **The sentinel is per-session, keyed on `SESSION_TAG`.** Never use a single shared `.end_pending` — with parallel sessions it races and can merge the wrong branch (the 2026-05-30 incident). Always recover `SESSION_TAG` from THIS conversation's scope first, and only ever merge/teardown that tag — even if an explicit `/end` re-invocation arrives with the sentinel missing.
- **With other active `fix-*` worktrees present, SKIP Step 7 (merge master into siblings) and Step 7.5b (auto-close "merged+clean" worktrees).** A freshly-forked sibling worktree (branch == master, clean tree) is classified "merged+clean" and would be wrongly torn down, destroying a parallel session's in-progress work. Siblings self-update via their own `/end` rebase (Step 1). Only the janitor + Step 7.5a (orphaned MERGED branches with no live worktree) are safe to run while siblings are active.
- **Never teardown the worktree without pre-unlinking the mingw junction** using the hardcoded backslash path — not `cygpath`.
- **Never auto-resolve rebase conflicts** — always stop and surface them.
- **Never commit `td5re.ini`, `log/`, or build artifacts**.
- **Never use `git branch -D`** during teardown — only `-d`. A worktree that wasn't merged should not be torn down by this skill; ask the user first.
- **If recheck surfaces uncertainties and the user chooses to work on them**: stop here, let the user work, and wait for `/end` to be re-invoked. Do NOT keep running the merge steps.
- **If `git worktree list` shows no active `fix-*` worktrees and we're in the main tree**: Steps 5–7 reduce to just a push + memory update. Skip teardown entirely. **Steps 7.5 and 7.6 still run** — 7.5 sweeps stray branch refs (`worktree-agent-*`, orphaned `fix-*`) and 7.6 commits any safe main-tree work + pushes, even when there are no worktrees left.
- **Step 7.5 never force-merges and never uses `git branch -D`.** It deletes only branches/worktrees fully contained in master; everything with unmerged work is reported for the user, never destroyed. This is the rule that prevents `/end` from silently swallowing an unfinished candidate fix.
- **`/end` always ends with `origin/master` in sync (Step 7.6).** It commits remaining *safe* main-tree work (explicit allowlist — never `git add -A`, never `td5re.ini`/saves/unknowns) and pushes as its final action. The session is not "done" until `origin/master..master` is empty. This is why you should never have to manually commit/push after a clean `/end`.
- **`/end` always builds BOTH binaries (Step 2d in-worktree as the recheck gate, Step 7.7 in main tree as the deliverable).** `build_all.bat` produces `td5re.exe` (dev, instrumented) and `td5re_release.exe` (release, stripped) side by side in the project root. A release-link `undefined reference to td5_pilot_trace_*` means a new ungated trace call site needs a `#ifndef TD5RE_RELEASE` guard — the dev build alone will not catch it. Never commit the `.exe`s or `build*/` dirs (gitignored); `td5re_release.ini` IS tracked (release config), `td5re.ini` is NOT (test scaffolding).
