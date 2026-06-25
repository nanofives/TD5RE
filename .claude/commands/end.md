# TD5RE End Session Workflow

Ship a `/fix` session in **one invocation**: rebase against master, resolve any conflicts *in this session*, recheck, then merge + push + tear down the worktree + delete the branch — no second invocation, no confirmation sentinel. The user asked for a single `/end` that does the whole thing; this skill delivers that while keeping every data-loss safeguard intact.

**Usage:** `/end` (run from the main tree or from inside any active `/fix` worktree)

**The contract:** `/end` is a commit point. When you invoke it, it merges. The only things that stop it are *hard safety failures* — a build that won't compile, a branch that touches forbidden paths (`original/**`, `re/**` outside the tracked whitelist), or a conflict so semantically irreconcilable that guessing would lose work. Stubs/TODOs/semgrep findings are **advisory**: logged to memory and reported, never a gate. There is no "first invocation pauses, second confirms" — that pattern was the main cause of slow merges and stranded worktrees, and it's gone.

---

## Step 0: Orient — detect context

Recover **this session's** `SESSION_TAG` + `WORKTREE_DIR`. This skill always operates on the worktree belonging to *this conversation*; other sessions' worktrees are not its concern.

1. If `SESSION_TAG` / `WORKTREE_DIR` are already in scope from a `/fix` session in this conversation, use those directly. **This is authoritative** — prefer it over anything on disk.
2. Otherwise infer from the current branch (valid only when `/end` runs from inside the worktree):

```bash
SESSION_TAG="$(git rev-parse --abbrev-ref HEAD)"
WORKTREE_DIR="$(git rev-parse --show-toplevel)"
echo "Session: ${SESSION_TAG}"
echo "Worktree: ${WORKTREE_DIR}"
```

**Main-tree-only path:** if `SESSION_TAG` resolves to `master` (run from the main tree, nothing in scope) and there are no active worktrees, skip Steps 1–6 entirely and jump to **Step 7c** (commit safe main-tree work + push). There is no branch to merge — `/end` just publishes any loose main-tree work and reports.

**Only ever merge/tear down the `SESSION_TAG` you recovered from THIS conversation.** Never act on a branch you can't tie to this session, even if a stray worktree or branch is lying around — those are swept (read-only, never destroyed if unmerged) in Step 7b.

> **Removed (2026-06-20):** the old `.end_pending.${SESSION_TAG}` sentinel + two-invocation gate. A single `/end` now does the full merge+teardown. Any legacy `.end_pending*` files are harmless and get swept in Step 7b.

---

## Step 1: Rebase onto origin/master (resolve conflicts in-session)

Bring the branch up to date with the true remote state before anything else. This is also where parallel-session conflicts surface — and per the user's instruction, **you resolve them here, now, in this conversation.** Do not punt them back to the user unless they are genuinely irreconcilable.

```bash
cd "${WORKTREE_DIR}"
git fetch origin master --quiet
REBASE_OUT="$(git rebase origin/master 2>&1)"; REBASE_EXIT=$?
echo "${REBASE_OUT}"
```

**If the rebase stops on a conflict (`REBASE_EXIT != 0`):** resolve it yourself.

### Conflict-resolution protocol (applies here and in Step 5)

1. `git status --short` to list conflicted files; `git diff` to read the hunks.
2. For each conflicted file, read **both** sides:
   - `<<<<<<< HEAD` … `=======` is the **incoming** side (master's change — the rebase replays your commits on top of it, so HEAD is master).
   - `=======` … `>>>>>>>` is **your branch's** change.
3. **Resolve by preserving both intents.** If the two changes are in independent regions, keep both. If they touch the same logic, *understand what each was doing* (use the commit messages, the original `$ARGUMENTS` fix description, and the surrounding code) and write the correct combined result — neither side's intent may be silently dropped.
4. `git add <file>` each resolved file, then `git rebase --continue`. Repeat until the rebase completes.
5. After the rebase finishes, the Step 2 build verifies the resolution compiles. If a resolution looks risky, build immediately before continuing.
6. **Never** blind-resolve with `-X ours/theirs` or `git checkout --ours/--theirs <whole-file>` unless you've confirmed the discarded side has no unique change. **Never** `git rebase --skip` a commit (that drops your work).
7. **Escalate to the user only** when both sides rewrote the same logic with genuinely incompatible intent and there is no correct combination — show the two versions and ask. This should be rare; most conflicts in this repo are independent edits to different functions.

**If the user explicitly aborts a genuinely-ambiguous conflict:** `git rebase --abort`, then ask whether to merge without rebasing (riskier — only if master hasn't moved) or stop. Default: stop and leave the worktree open.

---

## Step 2: Recheck (build once — this build IS the deliverable)

The recheck build happens **once, here, in the worktree**, and its exes are copied to the project root after the merge (Step 5). There is no post-merge rebuild — a clean rebase + clean `--no-ff` merge makes the merged main tree byte-identical to the rebased worktree HEAD, so the worktree's exes already are the shipped artifact.

### 2a. What changed?

```bash
cd "${WORKTREE_DIR}"
CHANGED_C="$(git diff --name-only origin/master...HEAD -- '*.c' '*.h')"
echo "Changed source files:"; echo "${CHANGED_C}"
```

### 2b. Advisory scans (NON-blocking — logged, never a gate)

```bash
cd "${WORKTREE_DIR}"
for f in ${CHANGED_C}; do
    [ -f "${f}" ] || continue
    echo "=== ${f} ==="
    grep -n -E 'TODO|FIXME|STUB|stub|placeholder|Placeholder|UNIMPLEMENTED|UNCERTAIN|\[UNCERTAIN\]|hardcoded.*TODO' "${f}" || true
done
if command -v semgrep >/dev/null 2>&1 && [ -n "${CHANGED_C}" ]; then
    semgrep --config=auto --lang=c --quiet ${CHANGED_C} 2>&1 | head -60
fi
```

Collect these for the Step 8 report and for memory (Step 3). They do **not** stop the merge.

### 2c. Record the session in the CHANGELOG + PENDING TO TEST seed (before the build)

Every shipped session must be reflected in the two in-game lists so players see what's new and the QA backlog tracks what still needs testing. `/fix` Step 2 normally does this per change, but `/end` is the backstop — it **guarantees** the session's user-visible work is logged before the deliverable exe is built. Idempotent: only add what's missing; never duplicate.

For each user-visible change in this session (derive from `${CHANGED_C}`, the commit messages, and the original `$ARGUMENTS`):

1. **CHANGELOG** — `${WORKTREE_DIR}/td5mod/src/td5re/td5_changelog.h`. If the change isn't already listed, add a player-facing, present-tense `{ CL_ITEM, "..." }` bullet at the **top** of today's date block under `LAST 7 DAYS` (create a `{ CL_BLANK, "" }` + `{ CL_DATE, "Month DD" }` block for today if none exists). Keep each line ≤ ~62 chars; wrap long items onto a continuation `{ CL_ITEM, "  ..." }` line with a leading two-space indent. Describe the player-visible effect, not the RE detail.
2. **PENDING TO TEST** — `${WORKTREE_DIR}/td5mod/src/td5re/td5_pending.c`, the `k_seed[]` array. If not already present, add one concise item string (≤ ~50 chars) near the **top** of the array. This seeds fresh checkouts (the runtime `td5re_pending.txt` is untracked, so `k_seed[]` is the durable home).

Both files are tracked and get committed in Step 4 and built into the exe in 2d. On the **main-tree-only path** (Step 0), make the same edits in the main tree instead — they'll be staged by Step 7c (`td5mod/src/td5re/*.c`/`*.h`) and shipped on the next build.

### 2d. Build BOTH binaries (the one hard gate)

```bash
cd "${WORKTREE_DIR}/td5mod/src/td5re"
./build_all.bat 2>&1 | tail -50
```

This produces `${WORKTREE_DIR}/td5re.exe` (dev) and `${WORKTREE_DIR}/td5re_release.exe` (release).

- **If either build fails:** fix the compile/link errors (up to 3 attempts), then re-run `build_all.bat`. A release-link `undefined reference to td5_pilot_trace_*` means a new ungated trace call site needs a `#ifndef TD5RE_RELEASE` guard — the dev build alone won't catch it.
- **If it still won't build after 3 attempts:** STOP. Report the error. **Do not merge broken code.** This is the one legitimate hard stop in `/end` — it's a safety failure, not a confirmation gate.

---

## Step 3: Defer stubs to memory (non-blocking)

For each stub/TODO grep hit from 2b, write a short memory entry so deferred work isn't lost. Group by theme (one file per cluster, not per line). This never blocks the merge.

```markdown
---
name: todo-<short-slug>
description: <one-line hook for MEMORY.md>
metadata:
  type: project
---

Deferred from session ${SESSION_TAG} (2026-06-20, absolute date).

<file>:<line> — <stub/TODO text>

**Why:** <reason, or "no time this session">.
**How to apply:** Ghidra-verify the original function first; port verbatim.
```

Write to `C:/Users/maria/.claude-account3/projects/C--Users-maria-Desktop-Proyectos-TD5RE/memory/todo_<slug>.md` and add a one-line pointer to `MEMORY.md` (check for an existing matching entry first — update rather than duplicate).

**Resolved TODOs:** for each changed file, if a TODO/stub present in `git show origin/master:<file>` is now gone, the session resolved it — mark the matching `MEMORY.md` entry RESOLVED (date + commit) or remove it.

---

## Step 4: Commit remaining worktree changes (source only)

```bash
cd "${WORKTREE_DIR}"
git status --short

# Stage source only — NEVER td5re.ini (test scaffolding), log/, or build artifacts.
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h 2>/dev/null || true
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true
git add td5mod/ddraw_wrapper/src/*.c td5mod/ddraw_wrapper/src/*.h 2>/dev/null || true
git add re/analysis/ re/sessions/ 2>/dev/null || true

# Forbidden-path guard.
BAD="$(git diff --cached --name-only | grep -E '^(original/|re/)' | grep -vE '^re/(analysis|sessions|assets/static/.*\.dat)$' || true)"
if [ -n "${BAD}" ]; then
    echo "REFUSING TO COMMIT — forbidden paths staged:"; echo "${BAD}"
    echo "Unstage with: git reset HEAD -- <path>"; exit 1
fi

if git diff --cached --quiet; then
    echo "Nothing staged — branch already fully committed."
else
    git commit -m "fix: <session work summary>

Session: ${SESSION_TAG}

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
fi
```

---

## Step 5: Merge into master under a lock — then copy exes + push

This is the critical section. A `mkdir`-atomic lock serializes it against any other `/end` running in a parallel session on this machine, so two sessions never manipulate the main tree's index/working-copy at the same time. Combined with a push-reject retry, this is what keeps concurrent sessions from "messing with master."

### 5a. Acquire the merge lock

```bash
LOCK="C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.merge.lock"
ACQUIRED=0
for attempt in $(seq 1 40); do
    if mkdir "${LOCK}" 2>/dev/null; then
        printf 'tag=%s pid=%s ts=%s\n' "${SESSION_TAG}" "$$" "$(date +%s)" > "${LOCK}/owner"
        ACQUIRED=1; break
    fi
    # Stale-steal: reclaim a lock older than 15 min (a crashed session).
    if [ -f "${LOCK}/owner" ]; then
        LOCK_TS="$(awk -F= '/ts=/{print $NF}' "${LOCK}/owner" 2>/dev/null | tr -dc 0-9)"
        NOW="$(date +%s)"
        if [ -n "${LOCK_TS}" ] && [ $((NOW - LOCK_TS)) -gt 900 ]; then
            echo "Reclaiming stale merge lock (owner: $(cat "${LOCK}/owner"))"
            rm -rf "${LOCK}"; continue
        fi
        echo "Another /end holds the merge lock ($(cat "${LOCK}/owner")) — waiting…"
    fi
    # No-sleep delay: foreground `sleep` is blocked by the harness; ping is the
    # portable ~3s wait on this host.
    ping -n 4 127.0.0.1 >/dev/null 2>&1 || true
done
[ "${ACQUIRED}" -eq 1 ] || { echo "Could not acquire merge lock after ~2 min — another session may be mid-merge. Re-run /end shortly."; exit 1; }
```

### 5b. Merge, copy exes, push (all inside the lock)

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Main tree must be clean before we touch it. If dirty from non-/fix work, STOP
# (release the lock first) and ask the user — never stash/discard their work.
if [ -n "$(git status --porcelain)" ]; then
    echo "Main tree dirty — refusing to merge. Commit/stash main-tree changes first."
    rm -rf "${LOCK}"; exit 1
fi

# Another session may have advanced origin/master while we built. Re-sync.
git fetch origin master --quiet
if [ "$(git rev-list --count master..origin/master)" -gt 0 ]; then
    echo "origin/master advanced during build — fast-forwarding local master + re-rebasing branch."
    git checkout master && git merge --ff-only origin/master
    git -C "${WORKTREE_DIR}" rebase origin/master   # resolve conflicts via the Step-1 protocol
    # Source changed under us → rebuild so the shipped exes match the merged tree.
    ( cd "${WORKTREE_DIR}/td5mod/src/td5re" && ./build_all.bat 2>&1 | tail -20 )
else
    git checkout master
fi

# Forbidden-path guard (pre-merge).
BAD_DIFF="$(git diff --name-status master.."${SESSION_TAG}" \
    | awk '$2 ~ /^(original\/|re\/)/ && $2 !~ /^re\/(analysis|sessions|assets\/static\/.*\.dat)$/ {print}')"
if [ -n "${BAD_DIFF}" ]; then
    echo "REFUSING TO MERGE — branch touches forbidden paths:"; echo "${BAD_DIFF}"
    rm -rf "${LOCK}"; exit 1
fi

# Precondition: irreplaceable data present.
test -d original && test -d re/assets || {
    echo "PRECONDITION FAILED: main tree missing original/ or re/assets/. Not merging."
    rm -rf "${LOCK}"; exit 1
}

git merge --no-ff "${SESSION_TAG}" -m "Merge ${SESSION_TAG}: <one-line session summary>"

# Post-merge guard: verify the merge didn't destroy irreplaceable data.
if [ ! -d original ] || [ ! -d re/assets ]; then
    echo "POST-MERGE FAILURE: original/ or re/assets/ disappeared. Reverting."
    git reset --hard ORIG_HEAD; rm -rf "${LOCK}"; exit 1
fi

# BUILD ONCE, DEPLOY BY COPY: the worktree already built the merged tree. Copy
# its exes to the project root instead of a redundant full rebuild in main.
cp "${WORKTREE_DIR}/td5re.exe"         C:/Users/maria/Desktop/Proyectos/TD5RE/td5re.exe         2>/dev/null || echo "WARN: dev exe not copied (worktree build missing?)"
cp "${WORKTREE_DIR}/td5re_release.exe" C:/Users/maria/Desktop/Proyectos/TD5RE/td5re_release.exe 2>/dev/null || echo "WARN: release exe not copied"

# Push with one reject-retry (covers a race where origin moved between fetch and push).
if ! git push origin master 2>&1; then
    echo "Push rejected — re-syncing and retrying once."
    git fetch origin master --quiet
    git merge --ff-only origin/master || { echo "origin diverged non-trivially — resolve manually."; rm -rf "${LOCK}"; exit 1; }
    git push origin master
fi

git fetch origin master --quiet
AHEAD="$(git rev-list --count origin/master..master)"
[ "${AHEAD}" -gt 0 ] && { echo "Push still pending — surface to user, do NOT report shipped."; rm -rf "${LOCK}"; exit 1; }
echo "origin/master in sync."
```

### 5c. Release the lock

```bash
rm -rf "${LOCK}" && echo "merge lock released."
```

Release the lock **before** teardown — teardown only touches this session's own worktree, which no other session contends for.

---

## Step 6: Tear down the worktree immediately (junction-safe)

Teardown runs **right after the merge** — before any sibling propagation, branch sweep, or pool sync. Once master has the commit, the worktree is disposable; nothing downstream may strand it. This is the same junction-safe protocol as `/fix`'s **Abandoning a worktree** block (see `fix.md` for the full TEARDOWN SAFEGUARDS rationale: deny-delete ACL on `original/` + `td5mod/deps/mingw/`, off-disk backup at `C:\Users\maria\Desktop\TD5RE_backup_2026-05-28\`, and the host-specific junction-unlink quirk).

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# CANARY (pre): irreplaceable main-tree markers must exist before we start.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "PRECONDITION: main-tree markers already missing — investigate first"; exit 1; }

# Pre-unlink the mingw junction. On THIS host `cmd //c "rmdir"` fails (volume-label
# syntax error, confirmed 2026-05-28), leaving the junction live for --force to
# follow. Use PowerShell's reparse-aware delete (link only, never the target).
powershell.exe -NoProfile -Command "\$p='${WORKTREE_DIR}/td5mod/deps/mingw' -replace '/','\\'; if (Test-Path \$p){ \$i=Get-Item \$p -Force; if (\$i.Attributes -band [IO.FileAttributes]::ReparsePoint){ \$i.Delete() } else { Write-Error 'NOT a reparse point — refusing'; exit 1 } }"

# MANDATORY: verify the junction is gone before --force.
if [ -d "${WORKTREE_DIR}/td5mod/deps/mingw" ]; then
    echo "ERROR: pre-unlink failed — junction still live. Do NOT run --force."; exit 1
fi

# Remove the worktree. If it fails because td5re.exe / a log is still locked
# (game window open), name the holder and retry — never leave it half-removed.
if ! git worktree remove --force "${WORKTREE_DIR}" 2>&1; then
    echo "git worktree remove failed — likely ${WORKTREE_DIR}/td5re.exe or log/ is still open."
    echo "Close the game window for this session, then this step retries:"
    powershell.exe -NoProfile -Command "Get-Process td5re* -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '*${SESSION_TAG}*' } | Select-Object Id,Path"
    ping -n 4 127.0.0.1 >/dev/null 2>&1 || true
    git worktree remove --force "${WORKTREE_DIR}" || {
        echo "Still locked. The merge is DONE and pushed — only cleanup remains. The worktree"
        echo "stays registered; the next /fix or /end Step 7b sweep will remove it once unlocked."
    }
fi
git branch -d "${SESSION_TAG}" 2>/dev/null || echo "(branch ${SESSION_TAG} not deleted — already gone or worktree still registered)"

# CANARY (post): the deny ACL should block any junction-follow, but verify.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "FATAL: main-tree data vanished during teardown — restore from C:\\Users\\maria\\Desktop\\TD5RE_backup_2026-05-28\\ NOW"; exit 1; }

# Stale-build janitor: sweep orphan numeric build_<pid>/ dirs from main (named
# variants like build_ttfonts/ are preserved).
shopt -s nullglob
for d in C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/build_[0-9]*/; do
    base=$(basename "$d"); [[ "$base" =~ ^build_[0-9]+$ ]] && rm -rf "$d"
done
shopt -u nullglob

# Screenshot scratch sweep (throwaway verification captures only — never re/assets,
# re/analysis, re/sessions, or any tracked path).
[ -d C:/Users/maria/Desktop/Proyectos/TD5RE/re/screenshots ] && rm -rf C:/Users/maria/Desktop/Proyectos/TD5RE/re/screenshots && echo "swept re/screenshots/"
```

Only `git branch -d` (never `-D`) — `-d` refuses an unmerged branch, which is the backstop. We just merged, so it succeeds; if it refuses, investigate, don't force.

---

## Step 7: Housekeeping (after teardown — never blocks the ship)

The merge is done and the worktree is gone. Everything below is cleanup; a failure here does not undo the ship. **Skip Step 7a and the worktree-closing half of 7b while other `fix-*` worktrees are active** — a freshly-forked sibling (branch == master, clean) looks "merged+clean" and would be wrongly torn down, destroying a parallel session's in-progress work. Siblings self-update via their own `/end` rebase.

### 7a. Propagate new master into sibling worktrees (skip if any sibling is active and mid-work)

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
for WT in .claude/worktrees/fix-*; do
    [ -d "${WT}" ] || continue
    git -C "${WT}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || continue
    echo "=== Propagating master into ${WT} ==="
    if ! MERGE_ERR="$(git -C "${WT}" merge --no-ff --no-edit master 2>&1)"; then
        echo "${MERGE_ERR}"
        if echo "${MERGE_ERR}" | grep -qiE "permission denied|unable to (unlink|create|write)|text file busy"; then
            echo "STOPPED: ${WT} has locked files. Abort partial merge + retry manually."
            git -C "${WT}" merge --abort 2>/dev/null || true; break
        fi
        echo "${MERGE_ERR}" | grep -qi "conflict" && { echo "STOPPED: conflict in ${WT} — resolve manually."; break; }
        echo "STOPPED: unexpected failure in ${WT}."; break
    fi
done
```

### 7b. Sweep stray branches + fully-merged worktrees (report-only for anything unmerged)

Deletes only what is **provably disposable** (fully contained in master); reports anything with unmerged commits or uncommitted changes for the user's decision. Never force-merges, never `git branch -D`.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
git fetch origin master --quiet 2>/dev/null || true
REPORT="$(mktemp)"
CHECKED_OUT="$(git worktree list --porcelain | awk '/^branch /{sub(/^refs\/heads\//,"",$2);print $2}')"

# Stray branch refs (no live worktree): worktree-agent-* (subagent leftovers) and
# fix-* whose worktree is gone.
for B in $(git for-each-ref --format='%(refname:short)' 'refs/heads/worktree-agent-*' 'refs/heads/fix-*'); do
    [ "${B}" = "master" ] && continue
    printf '%s\n' "${CHECKED_OUT}" | grep -qx "${B}" && continue
    if git merge-base --is-ancestor "${B}" master 2>/dev/null; then
        git branch -d "${B}" >/dev/null 2>&1 && echo "deleted merged branch: ${B}"
    else
        AHEAD="$(git rev-list --count "master..${B}" 2>/dev/null || echo '?')"
        echo "BRANCH   ${B} — ${AHEAD} commit(s) not in master" >> "${REPORT}"
    fi
done

# Legacy/orphaned sentinels (from the retired two-invocation design) + the merge
# lock if stale. Safe to remove once the matching worktree is gone.
for S in .claude/worktrees/.end_pending .claude/worktrees/.end_pending.*; do
    [ -e "${S}" ] || continue
    TAG="${S#.claude/worktrees/.end_pending.}"
    if [ "${S}" = ".claude/worktrees/.end_pending" ] || [ ! -d ".claude/worktrees/${TAG}" ]; then
        rm -f "${S}" && echo "removed stale sentinel: ${S}"
    fi
done

# Fully-merged + clean sibling worktrees (skip while siblings are active — see the
# Step 7 banner). Only auto-close merged+clean; report dirty/unmerged.
git worktree list --porcelain | awk '
    /^worktree /{wt=$2} /^branch /{sub(/^refs\/heads\//,"",$2);br=$2}
    /^$/{if(wt)printf "%s\t%s\n",wt,br; wt="";br=""} END{if(wt)printf "%s\t%s\n",wt,br}' \
| while IFS=$'\t' read -r WT BR; do
    case "${WT}" in */.claude/worktrees/fix-*) ;; *) continue ;; esac
    DIRTY="$(git -C "${WT}" status --porcelain 2>/dev/null | wc -l)"
    if [ -n "${BR}" ] && git merge-base --is-ancestor "${BR}" master 2>/dev/null && [ "${DIRTY}" -eq 0 ]; then
        echo "SAFE-TO-CLOSE ${WT} (${BR}) — merged + clean; tear down with the Step 6 block." >> "${REPORT}"
    else
        WHY=""; [ "${DIRTY}" -gt 0 ] && WHY="dirty=${DIRTY} "
        git merge-base --is-ancestor "${BR}" master 2>/dev/null || WHY="${WHY}unmerged"
        echo "WORKTREE ${WT} (${BR}) — ${WHY}" >> "${REPORT}"
    fi
done

git worktree prune --verbose

if [ -s "${REPORT}" ]; then
    echo ""; echo "WORKTREE / BRANCH LEFTOVERS:"; sed 's/^/  /' "${REPORT}"
    echo "  → SAFE-TO-CLOSE entries: run the Step 6 junction-safe teardown (plug WORKTREE_DIR + SESSION_TAG)."
    echo "  → unmerged/dirty: left untouched — resume with /fix, or delete manually once sure."
fi
rm -f "${REPORT}" 2>/dev/null || true
```

**For each SAFE-TO-CLOSE entry** (only when no sibling is actively mid-work), tear it down with the **Step 6 block verbatim** — plug `WORKTREE_DIR=<WT>`, `SESSION_TAG=<BR>`. Never `rm -rf` a worktree path.

---

## Step 7c: Commit safe main-tree work + guaranteed final push

`/end` must leave the repo fully committed and pushed. Step 5 handled the worktree branch; this commits main-tree-only work (skill edits, `.gitignore`, RE docs, tooling) that was never part of a `/fix` branch, then makes the final push. **This step also runs on the main-tree-only path from Step 0.**

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Stage by EXPLICIT allowlist. NEVER `git add -A` / `git add .` (that committed a
# runtime Config.td5 save on 2026-05-29).
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h 2>/dev/null || true
git add td5mod/src/td5re/*.bat 2>/dev/null || true
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true
git add td5mod/ddraw_wrapper/src/*.c td5mod/ddraw_wrapper/src/*.h 2>/dev/null || true
git add .claude/commands/*.md 2>/dev/null || true
git add re/analysis/ re/sessions/ 2>/dev/null || true
git add scripts/ 2>/dev/null || true
git add td5re_release.ini 2>/dev/null || true        # NOT td5re.ini (test scaffolding)
git add .gitignore CLAUDE.md AGENTS.md 2>/dev/null || true

BAD="$(git diff --cached --name-only | grep -E '^(original/|re/)' | grep -vE '^re/(analysis|sessions|assets/static/.*\.dat)$' || true)"
[ -n "${BAD}" ] && { echo "REFUSING — forbidden paths staged:"; echo "${BAD}"; exit 1; }

# Surface (never auto-commit) anything outside the allowlist.
LEFT="$(git status --short | grep -vE '^[MARD]  ' || true)"
[ -n "${LEFT}" ] && { echo "Left in working tree (review/commit manually if wanted):"; echo "${LEFT}" | sed 's/^/  /'; }

if git diff --cached --quiet; then
    echo "No main-tree changes to commit."
else
    git commit -m "chore(session): commit remaining main-tree work — ${SESSION_TAG}

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
fi

# Guaranteed final push — the LAST git action of /end.
git push origin master
git fetch origin master --quiet
AHEAD="$(git rev-list --count origin/master..master)"
if [ "${AHEAD}" -gt 0 ]; then
    echo "POST-CONDITION FAILED: master is ${AHEAD} ahead of origin. Retry push or surface the error —"
    echo "do NOT report shipped while commits are unpushed."; exit 1
fi
echo "origin/master in sync — session fully published."
```

---

## Step 7d: Final Ghidra pool sync (if RE annotations shipped)

```bash
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" cleanup
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" sync
```

Always safe to re-run (locked slots are logged-skipped). Failure here is non-fatal — the fix is already shipped. Never run `ghidra_pool.sh init` (wipes in-progress pool edits).

---

## Step 7e: Publish the release to the LAN server (non-blocking)

**Every `/end` is a release.** The fresh `td5re_release.exe` (copied to the project root in Step 5b) plus `re/assets/` are mirrored to the Pi at `mariano-server.local:8088`, so `update.bat` on every game machine pulls the new build on its next run. `deploy_release.sh` regenerates `manifest.json`, diffs it against the server, and uploads **only changed files** (incremental — a code-only session ships just the exe + manifest), then restarts the nginx host.

This runs **after** the push — the ship is already done in Step 7c. A failure here (Pi offline, SSH key absent, network down) is **non-fatal**: it is reported and the session still closes shipped. Set `TD5RE_SKIP_PUBLISH=1` to skip entirely (e.g. when working away from the LAN).

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
if [ "${TD5RE_SKIP_PUBLISH:-0}" = "1" ]; then
    PUBLISH="skipped (TD5RE_SKIP_PUBLISH=1)"
    echo "${PUBLISH}"
elif bash scripts/deploy_release.sh; then
    PUBLISH="published to mariano-server.local:8088 — game machines get it on next update.bat"
else
    RC=$?
    if [ "${RC}" -eq 2 ]; then
        PUBLISH="SKIPPED — Pi unreachable. Run 'bash scripts/deploy_release.sh' when it's online."
    else
        PUBLISH="FAILED (rc=${RC}) — ship is safe; re-run 'bash scripts/deploy_release.sh' manually."
    fi
    echo "${PUBLISH}"
fi
```

A non-zero result here **never** undoes the ship — `/end` already merged and pushed in Step 7c. The publish is best-effort mirroring on top of an already-completed release.

---

## Step 7f: Prune tested items from the pending-test checklist (non-blocking)

The **PENDING TO TEST** menu (dev/QA feature) writes `td5re_pending.txt` at the
project root — untracked runtime state, one item per line: `[x] ...` = tested/done,
`[ ] ...` = still to test, `# ...` = comment. `/end` drops the done items so the
list shrinks to what still needs testing. Non-blocking: a missing file or any error
never undoes the ship.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
PF="td5re_pending.txt"
if [ -f "${PF}" ]; then
    DONE="$(grep -c '^\[[xX]\]' "${PF}" 2>/dev/null)"; [ -z "${DONE}" ] && DONE=0
    if [ "${DONE}" -gt 0 ]; then
        grep -v '^\[[xX]\]' "${PF}" > "${PF}.tmp" && mv "${PF}.tmp" "${PF}"
        echo "pending-test: pruned ${DONE} tested item(s) from ${PF}"
    else
        echo "pending-test: nothing marked tested — list unchanged"
    fi
else
    echo "pending-test: no ${PF} yet (game hasn't written one) — skipped"
fi
```

---

## Step 8: Session close report

```
SESSION SHIPPED — ${SESSION_TAG}
================================
Merged:   yes (merge <hash>) / skipped (main-tree-only)
Conflicts resolved this session: <files + one-line how, or 'none'>
Push:     origin/master in sync
Worktree: torn down / still registered (locked — will sweep next /end)
Binaries: td5re.exe + td5re_release.exe copied to project root from the worktree build
Published: ${PUBLISH}   (LAN server — game machines update via update.bat)

Deferred to memory:    <new TODO entries, or 'none'>
Resolved this session:  <resolved TODOs, or 'none'>
Advisory findings:      <stub/semgrep notes, or 'none'>  (did NOT block merge)
Cleaned up (Step 7b):   <branches deleted + worktrees auto-closed, or 'none'>
Leftovers (your call):  <REPORT list, or 'none'>
Open worktrees:         <git worktree list, fix-* only>

Next: <run /fix for deferred stubs | resume a leftover | nothing pending>
```

---

## Key Rules

- **One invocation ships it.** `/end` rebases, resolves conflicts in-session, recheck-builds, merges, copies the exes, pushes, tears down, and deletes the branch — all in a single run. No sentinel, no second invocation, no "re-run to confirm." (The two-invocation gate was retired 2026-06-20 as the main cause of slow merges + stranded worktrees.)
- **Only three things stop the merge:** (1) a build that won't compile after 3 fix attempts, (2) a branch touching forbidden paths (`original/**`, `re/**` outside `re/{analysis,sessions,assets/static/*.dat}`), (3) a conflict so semantically irreconcilable that resolving would lose work. Everything else (stubs, TODOs, semgrep) is advisory — logged and reported, never gating.
- **You resolve conflicts, the user doesn't.** On any rebase/merge conflict, read both sides, preserve both intents, build to verify. Escalate only on genuine incompatible-intent conflicts. Never `-X ours/theirs` blind, never `git rebase --skip`.
- **Merge happens under a `mkdir` lock** so parallel-session `/end`s serialize on the main tree, plus a push-reject retry for the origin-moved race. Release the lock before teardown.
- **Build once, deploy by copy.** The worktree recheck-build IS the deliverable; its exes are copied to the project root post-merge. No redundant post-merge rebuild. (If origin moved during the build and we re-rebased, we rebuild inside the lock so the copied exes match the merged tree.)
- **Teardown is immediate and junction-safe.** Right after push, before housekeeping. Always pre-unlink the mingw junction with the PowerShell reparse-delete (never `cmd //c rmdir` on this host), verify it's gone, only then `git worktree remove --force`, with pre/post canaries. Never `rm -rf` a worktree path. Never `git branch -D` here.
- **Housekeeping never blocks the ship and never destroys unmerged work.** Step 7 runs after the worktree is gone; it deletes only branches/worktrees fully contained in master and reports everything else. Skip the worktree-closing parts while sibling `fix-*` worktrees are active.
- **`/end` ends with `origin/master..master` empty.** The final push (Step 7c) is the last git action. Never report shipped while commits are unpushed.
- **Every `/end` auto-publishes the release to the LAN server (Step 7e).** After the push, `deploy_release.sh` mirrors `td5re_release.exe` + `re/assets/` to the Pi (`mariano-server.local:8088`, incremental upload). Non-blocking: a Pi-offline/SSH failure (clean exit 2) or any other error is reported, never undoes the ship. Skip with `TD5RE_SKIP_PUBLISH=1`. This is what keeps the downstairs game machines from going stale — they pull the new build via `update.bat`.
- **Never commit `td5re.ini`, `log/`, build artifacts, or `*.td5` saves.** Stage by explicit allowlist only — never `git add -A`/`.`. `td5re_release.ini` IS tracked; `td5re.ini` is NOT.
- **Every session updates the in-game CHANGELOG + PENDING TO TEST seed (Step 2c), before the build.** Each user-visible change gets a present-tense bullet in `td5_changelog.h` (top of today's `LAST 7 DAYS` block) and a concise test item in `k_seed[]` in `td5_pending.c`. `/fix` adds these per change; `/end` is the idempotent backstop (and the only path on a main-tree-only `/end`). The runtime `td5re_pending.txt` is untracked — `k_seed[]` is the durable home.
