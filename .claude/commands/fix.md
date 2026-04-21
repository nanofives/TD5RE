# TD5RE Fix Workflow

Fix a bug or implement a feature in the TD5RE source port. Delegates Ghidra RE research to a subagent (keeps large MCP responses out of main context), then implements the fix **inside an isolated git worktree** so parallel `/fix` sessions can't clobber each other's `td5re.exe`, `td5re.ini`, or `log/`. On user approval the worktree is merged back and deleted.

**Usage:** `/fix <description of the bug or feature>`

## Workflow

### Step 0: Spin up an isolated test worktree

Every `/fix` run gets its own git worktree under `.claude/worktrees/` so builds, logs, and INI tweaks stay isolated from the main tree and from other concurrent sessions.

Pick a short unique session tag and create the worktree from `master`. Run these from the main tree (`C:/Users/maria/Desktop/Proyectos/TD5RE`):

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Unique tag (epoch + PID). Keep it short — also becomes the branch name.
SESSION_TAG="fix-$(date +%s)-$$"
WORKTREE_DIR="C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/${SESSION_TAG}"

# Create a new branch from master in its own worktree.
git worktree add -b "${SESSION_TAG}" "${WORKTREE_DIR}" master

# All subsequent edits / builds / runs happen inside $WORKTREE_DIR.
echo "Worktree ready at: ${WORKTREE_DIR}"
echo "Branch: ${SESSION_TAG}"
```

**Record the `WORKTREE_DIR` and `SESSION_TAG`** — you will use them for every Read/Edit/Bash call for the rest of the workflow. From here on:

- All source-file Read/Edit calls target paths **under `${WORKTREE_DIR}`**, not the main tree.
- Build, run, and log-read commands `cd` into `${WORKTREE_DIR}`.
- The td5re.ini you edit is `${WORKTREE_DIR}/td5re.ini` (it inherits the master copy).
- Built `td5re.exe` lands at `${WORKTREE_DIR}/td5re.exe`. Logs land in `${WORKTREE_DIR}/log/`.
- The Ghidra pool is **shared** across sessions — no change there.

If `git worktree add` fails because the tag is already in use, pick a new tag and retry.

### Step 1: Ghidra Research Agent

Launch a **general-purpose Agent** (has MCP access) to investigate the original binary. The agent prompt MUST include:

- The bug/feature description: `$ARGUMENTS`
- Instructions to connect to the analyzed Ghidra project using the **pool system**:
  ```
  STEP A: Acquire a pool slot by running this Bash command:
    bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" acquire

  This prints a project name like "TD5_pool2". If it prints an ERROR (all locked),
  run: bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" cleanup
  then retry acquire. If still failing, fall back to master TD5.

  STEP B: Open the acquired slot:
    mcp__ghidra__project_program_open_existing with:
      project_location="C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_pool"
      project_name=<the name from Step A, e.g. "TD5_pool2">
      program_name="TD5_d3d.exe"
      read_only=true

  If STEP B fails with LockException, go back to STEP A (the acquire script
  already skips locked slots, so this means a race — just retry).

  FALLBACK: If the pool directory doesn't exist, use the master project directly:
    mcp__ghidra__project_program_open_existing with:
      project_location="C:/Users/maria/Desktop/Proyectos/TD5RE"
      project_name="TD5"
      program_name="TD5_d3d.exe"
      read_only=true
  ```
- Instructions to find and decompile the relevant function(s) using `mcp__ghidra__function_by_name` or `mcp__ghidra__search_text`, then `mcp__ghidra__decomp_function`
- Instructions to trace callers/callees with `mcp__ghidra__function_callees` / `mcp__ghidra__function_callers` if needed
- Instructions to read the current source port file(s) under `${WORKTREE_DIR}/td5mod/src/td5re/` for comparison (pass the resolved worktree path — NOT the main tree — so the agent compares against the same files you'll be editing)
- **NO-GUESSING RULE** (CRITICAL — include verbatim in every research agent prompt):
  ```
  You are doing deterministic reverse engineering. Follow these rules strictly:
  - Report ONLY what the decompilation literally shows. Never infer, speculate, or "fill in gaps."
  - For every constant, offset, or formula you report: include the exact Ghidra address where it appears.
  - If a value's meaning is unclear, report it as a raw hex/decimal constant — do NOT assign semantic meaning.
  - If a function's purpose is unclear, describe it mechanically (reads offset X, calls Y, writes Z) — do NOT guess its intent or name it.
  - If the connection between the bug and a function is unclear, say "CONNECTION UNCLEAR" — do NOT fabricate a causal chain.
  - Never use words like "probably", "likely", "seems to", "appears to", "I think", "presumably."
  - If something is uncertain, mark it [UNCERTAIN] and state exactly what evidence is missing.
  - When reporting sign-sensitive values (coordinates, offsets, velocities): always report BOTH the raw hex AND the signed decimal interpretation.
  ```
- **Return format**: The agent must return a COMPACT summary (under 600 words) containing:
  1. Which original function(s) were decompiled (name + address)
  2. The extracted logic: exact constants (hex + decimal), formulas (with addresses), state transitions, coordinate math
  3. What the source port currently does differently (with file paths and line numbers)
  4. Specific changes needed to fix the bug
  5. **Per-item confidence** — For EVERY constant, formula, offset, or behavioral claim, tag it individually:
     - `[CONFIRMED @ 0xADDR]` — directly visible in decompilation at that address
     - `[INFERRED]` — not directly in decompilation but derived from surrounding context (explain how)
     - `[UNCERTAIN]` — ambiguous, multiple valid interpretations exist (list them)
     - `[GUESSED]` — no direct evidence, filling a gap (explain what evidence is missing)
  6. **Blit mode** (if frontend/HUD related): for each TGA surface drawn, report the `Copy16BitSurfaceRect` flag — `0x10` = opaque blit, `0x11` = color-key transparent blit
  7. **Asset dimensions** (if any TGA/surface is involved): read actual TGA header dimensions — do NOT derive from `create_surface` calls (those are compositing buffers)
  8. **Unknowns list**: Explicitly list anything the research could NOT determine — do not silently omit gaps

After all Ghidra MCP calls are done, clean up locks and refresh the pool:
```bash
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" cleanup
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" sync
```
This ensures other sessions pick up any master project changes. Slots locked by other active sessions are safely skipped.

Do NOT ask the agent to make edits. Research only.

### Step 1 Gate: Ghidra Access Required (HARD STOP)

**Before proceeding to Step 1.5 or any code changes, verify the research agent's output:**

If the research agent returned ANY of the following:
- Could not open the Ghidra project (lock timeout, connection error, MCP unavailable)
- Could not find or decompile the relevant function(s)
- Returned zero confirmed decompilation facts (no `[CONFIRMED @ 0xADDR]` items)
- Returned only `[GUESSED]` or `[INFERRED]` items with no direct decompilation evidence

**→ STOP IMMEDIATELY. Do not proceed to Step 1.5 or Step 2. Do not make any code changes.**

Output this exact message to the user and wait for their input:
```
STOPPED: Ghidra research did not return confirmed decompilation data.

Reason: <paste the research agent's failure reason>

Cannot implement a fix without verified RE basis. Options:
1. Open the Ghidra project manually and retry (/fix again once it's unlocked)
2. Provide a function name or address hint so the agent can search more specifically
3. Use Frida/x32dbg to observe runtime behavior instead
```

**Never fall back to guessing, "reasonable assumptions", or source-port-only analysis when Ghidra is unavailable.**

---

### Step 1.5: Validate Research (before coding)

Run this checklist against the research output. If any item fails, re-query the research agent before coding.

1. **Sign check**: Any constant > `0xF0000000` in a coordinate/offset/velocity context? Interpret as negative (e.g., `0xFFFFFFD8` = `-40`).
2. **ESI/EDI swap check**: If coordinates from ESI/EDI in a multi-state FSM: verify `ESI = Y-base`, `EDI = X-base`. Agents consistently swap these — if a rect goes off-screen, try swapping x/y.
3. **TGA dimension check**: If the fix involves any TGA asset, verify actual file dimensions:
   ```bash
   python3 -c "import struct; d=open('path/to/file.tga','rb').read(); print('WxH:', struct.unpack_from('<HH',d,12))"
   ```
   Never trust `create_surface(w,h)` calls from Ghidra — those are compositing buffers.
4. **Blit mode check**: Confirm opaque (`0x10`) vs color-key (`0x11`). UI chrome = opaque. Car/track previews = color key.
5. **Atlas UV check**: Static atlas pages are 256×256 BGRA32. UV normalization = ÷256.0f for both U and V.
6. **Fixed-point check**: 24.8 format — negative velocities look like large unsigned values (`0xFFFFFF00` = `-1.0`).

### Step 2: Implement (inside the worktree)

Using the research summary from Step 1, make the code changes **in the worktree you created in Step 0** — never in the main tree.

1. Edit the relevant source files under `${WORKTREE_DIR}/td5mod/src/td5re/` (Edit tool with absolute paths under the worktree).
2. Run the build inside the worktree. The build script already handles its own output; because we're in a worktree, `td5re.exe` lands at `${WORKTREE_DIR}/td5re.exe` and can't stomp on another session's exe in the main tree:
   ```bash
   cd "${WORKTREE_DIR}/td5mod/src/td5re" && ./build_standalone.bat $$
   ```
   (`$$` expands to the shell PID, giving a unique `build_<pid>/` intermediate dir inside this worktree)
3. Fix any compile errors (up to 2 attempts).
4. Run the **single-track runtime probe** (see [Testing the build](#testing-the-build-ini--log-loop)) — fast sanity check on Moscow before the multi-track sweep.
5. **Run the multi-track CSV bundle sweep** (see [Multi-track CSV bundle sweep](#multi-track-csv-bundle-sweep-mandatory)) — this is **mandatory** for any fix touching race / AI / physics / camera / track / HUD code. Skip only when the fix is pure frontend/asset/build-tooling.
6. Report the result to the user: what was found, what was changed, build status, the worktree path so they can inspect or manually launch, AND the bundle path at `tools/frida_csv/${SESSION_TAG}/`.

#### MANDATORY: Verbose Uncertainty Disclosure

**Every decision you make during implementation MUST be annotated with its confidence basis.** This is non-negotiable. Too many fixes have been wrong because uncertain decisions were presented as fact.

Before writing ANY line of code, explicitly state to the user:

1. **CONFIRMED decisions** — backed by exact Ghidra addresses, exact constants, or exact decompiled logic. State the evidence. These you implement silently.

2. **UNCERTAIN decisions** — ANY of the following MUST be called out **loudly and explicitly** before coding:
   - A value where the decompilation was ambiguous (e.g., sign, shift direction, cast width)
   - A formula you reconstructed but the Ghidra output didn't show it cleanly (e.g., optimized-out intermediates, inlined operations)
   - An assumption about which variable maps to which struct field
   - A guess about execution order, state machine transitions, or conditional branches
   - Any place where you chose one interpretation over another plausible one
   - Coordinate system assumptions (which axis is X vs Y, sign conventions, origin location)
   - Integer width/sign interpretation (signed vs unsigned, 16-bit vs 32-bit)
   - Whether a bitmask/flag is AND vs OR, or its exact bit position
   - Array index base (0-based vs 1-based) when not explicitly clear from decompilation

3. **Format for uncertain decisions** — Use this exact format in your response to the user for EACH uncertain item:
   ```
   [UNCERTAIN] <what you're deciding>
   - What the decompilation shows: <raw facts>
   - What I'm choosing to do: <your interpretation>
   - Why: <reasoning>
   - Alternative interpretation: <what else it could be>
   - Risk if wrong: <what would visibly break>
   ```

4. **If more than 3 items are UNCERTAIN**: Stop and ask the user whether to proceed or investigate further (Frida, x32dbg, runtime test) before writing code.

5. **Never say "based on the decompilation" without citing the specific address and the exact bytes/operations at that address.** Vague attribution to "the decompilation" is not allowed.

### Step 3: Commit to the worktree branch (isolated)

After the build passes and the runtime test (below) looks sane, commit the change **to the worktree's branch only** — do NOT touch master or push yet. This keeps the fix isolated so the user can test it against other parallel `/fix` candidates before merging.

```bash
cd "${WORKTREE_DIR}"

# Stage only source files (never stage build outputs, logs, or game data)
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true

# HARD GUARD: original/ and re/ must NEVER be part of a /fix commit. They are
# gitignored at the root level, but a bug or stray `git add` could sneak them
# in — and because they're junctioned from main, staging a deletion here means
# the merge will wipe the real data in the main tree.
# Whitelist for re/: only re/assets/static/**.dat is legitimately tracked.
BAD="$(git diff --cached --name-only | grep -E '^(original/|re/)' | grep -vE '^re/assets/static/.*\.dat$' || true)"
if [ -n "${BAD}" ]; then
    echo "REFUSING TO COMMIT — forbidden paths staged:"
    echo "${BAD}"
    echo ""
    echo "original/ and re/ (except re/assets/static/*.dat) must never be part of a /fix commit."
    echo "Unstage them with: git reset HEAD -- <path>"
    exit 1
fi

git commit -m "fix: <one-line description from $ARGUMENTS>

RE basis: <function name(s) and Ghidra address(es) from Step 1>
CSV bundle: tools/frida_csv/${SESSION_TAG}/  (Moscow + Newcastle + <random track name>)

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

Rules for this step:
- Use `git add` with explicit file paths, never `git add .` or `git add -A`.
- The commit subject must start with `fix:` and match the original `$ARGUMENTS` description.
- The body must cite the RE basis AND the bundle path from the multi-track sweep so a future bisect can reproduce the runs.
- **Do NOT push and do NOT merge to master yet.** The whole point of the worktree is that multiple candidate fixes can coexist unmerged.
- Report to the user: the worktree path, the branch name, what was changed, how to run it (`${WORKTREE_DIR}/td5re.exe`), AND the bundle dir. End with an explicit prompt: *"Merge this into master and delete the worktree? (yes / keep for more testing)"*.

### Step 4: Merge into master + delete worktree (only on user approval)

**Wait for an explicit "yes / merge / ship it" from the user.** Do NOT run this step automatically after Step 3. The user typically iterates across several worktrees before deciding which one is the keeper.

When the user approves, merge the branch back into master in the main tree, push, then tear down the worktree:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Make sure the main tree is clean before merging. If there are uncommitted
# changes here from non-/fix work, STOP and ask the user — do not stash or
# discard their work.
git status --porcelain

# HARD GUARD (pre-merge): abort if the branch would delete or modify anything
# under original/ or re/ (outside re/assets/static/*.dat). These paths are
# junctions or large tracked/runtime data and must never be touched by a /fix.
BAD_DIFF="$(git diff --name-status master.."${SESSION_TAG}" \
    | awk '$2 ~ /^(original\/|re\/)/ && $2 !~ /^re\/assets\/static\/.*\.dat$/ {print}')"
if [ -n "${BAD_DIFF}" ]; then
    echo "REFUSING TO MERGE — branch ${SESSION_TAG} touches forbidden paths:"
    echo "${BAD_DIFF}"
    echo ""
    echo "These paths (original/**, re/** outside re/assets/static/*.dat) must"
    echo "never be modified by a /fix branch. Inspect the branch manually before"
    echo "deciding what to do."
    exit 1
fi

# Remember where original/ and re/ currently point so we can post-verify.
test -d original && test -d re/assets || {
    echo "PRECONDITION FAILED: main tree is missing original/ or re/assets/."
    echo "Do NOT merge into a broken main tree. Investigate first."
    exit 1
}

# Merge the worktree branch. Use --no-ff so the worktree's history stays
# visible as a single logical fix, which helps when bisecting later.
git merge --no-ff "${SESSION_TAG}" -m "Merge ${SESSION_TAG}: <one-line description>"

# HARD GUARD (post-merge): verify the merge didn't destroy original/ or re/.
# If it did, revert the merge commit before pushing so main stays intact.
if [ ! -d original ] || [ ! -d re/assets ]; then
    echo "POST-MERGE FAILURE: original/ or re/assets/ disappeared after merge."
    echo "Reverting the merge commit to protect the main tree."
    git reset --hard ORIG_HEAD
    echo "Main tree restored. Do NOT push. Investigate ${SESSION_TAG} before retrying."
    exit 1
fi

git push origin master

# Tear down: remove the worktree directory, then delete the merged branch.
git worktree remove "${WORKTREE_DIR}"
git branch -d "${SESSION_TAG}"
```

Rules for this step:
- If `git merge` reports conflicts (another session merged something that collides), STOP and report the conflicting files to the user — do NOT auto-resolve.
- If `git merge` reports `Your local changes ... would be overwritten by merge` (main tree has uncommitted work), STOP and report the file list. Do NOT stash or discard. The user must commit/clean main first; then either retry the merge, or — if the fix branch was created from old master and the merge would back out unrelated work — fall back to `git cherry-pick <fix-commits>` onto current master. Either path STILL requires the same post-merge guard, push, and teardown below.
- If `git worktree remove` fails because files are locked (e.g., `td5re.exe` still running), ask the user to close the game, then retry. Do NOT use `--force` without confirmation.
- If `git push` fails (no remote, auth error, etc.), report the error to the user and stop — do not retry destructively. The merge commit is already in local master, so there's nothing to clean up.
- Only delete the branch with `-d` (safe, rejects unmerged branches). Never use `-D`.
- **Push is mandatory.** Whether the fix landed via `git merge --no-ff` or via fallback `git cherry-pick`, the workflow is not done until `git push origin master` succeeds and `git rev-list --count origin/master..master` returns `0`. Verify both before declaring the fix shipped.

### Step 4.5: Propagate new master into sibling /fix worktrees

After Step 4 merges the approved branch into master, every OTHER active `/fix` worktree is now one commit behind. Auto-fast-forward new master into each sibling worktree so they're all testing against the same base.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Enumerate every sibling fix-* worktree. The one just torn down is already gone
# from the list after Step 4's `git worktree remove`.
for WT in .claude/worktrees/fix-*; do
    [ -d "${WT}" ] || continue

    # Skip anything that's no longer a valid worktree entry (stale dir).
    if ! git -C "${WT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        continue
    fi

    echo "=== Propagating master into ${WT} ==="

    # Run the merge. --no-edit uses the default merge message; --no-ff preserves
    # the incoming history cleanly. Capture stderr so we can pattern-match locks.
    if ! MERGE_ERR="$(git -C "${WT}" merge --no-ff --no-edit master 2>&1)"; then
        echo "${MERGE_ERR}"

        # Detect a file-lock failure (running td5re.exe, open log, etc.). Windows
        # reports these as "Permission denied" / "unable to unlink" / "Invalid
        # argument" during checkout.
        if echo "${MERGE_ERR}" | grep -qiE "permission denied|unable to (unlink|create|write)|text file busy|invalid argument"; then
            echo "STOPPED: ${WT} has locked files (likely td5re.exe or log/ still open)."
            echo "Aborting the partial merge so the worktree is left clean."
            git -C "${WT}" merge --abort 2>/dev/null || true
            echo "Retry manually once testing stops:"
            echo "  cd \"${WT}\" && git merge --no-ff --no-edit master"
            break
        fi

        # Conflict (non-lock). Leave the conflict in place so the user can
        # resolve it, and stop the loop — propagating further would compound
        # confusion.
        if echo "${MERGE_ERR}" | grep -qi "conflict"; then
            echo "STOPPED: merge conflict in ${WT}. Resolve manually:"
            echo "  cd \"${WT}\" && git status    # see conflicting files"
            echo "  # fix, git add <files>, git commit"
            break
        fi

        # Anything else (detached HEAD, missing branch, etc.) — stop and surface.
        echo "STOPPED: unexpected merge failure in ${WT}. See error above."
        break
    fi
done
```

Rules for this step:
- **Any lock → stop immediately, abort that worktree's partial merge, and report.** Do not move on to other worktrees — the user asked to retry manually once the running test closes.
- **Any conflict → stop and leave the conflict in place** for manual resolution. Do not `--abort` in the conflict case; the user wants to see the diff.
- Never pass `--force` or `-X theirs/ours`. If a merge needs strategy help, that's a user decision.
- Don't touch worktrees outside `.claude/worktrees/fix-*` (the main tree is already updated, other unrelated worktrees are not our business).
- If the loop breaks early due to a lock/conflict, later worktrees stay unmerged — that's fine. Running this step again after the blocker clears will pick them up (git skips worktrees that are already up to date).

### Step 5: Post-condition — verify origin is in sync (HARD STOP)

After Step 4.5, run a final check that local master is fully published. This catches any case where the push was skipped — e.g. fallback cherry-pick path (Step 4 rules), partial flow due to manual recovery, or auth failure that wasn't surfaced.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
git fetch origin master --quiet
AHEAD="$(git rev-list --count origin/master..master)"
if [ "${AHEAD}" -gt 0 ]; then
    echo "POST-CONDITION FAILED: local master is ${AHEAD} commit(s) ahead of origin."
    echo "Pushing now:"
    git push origin master
    AHEAD="$(git rev-list --count origin/master..master)"
    if [ "${AHEAD}" -gt 0 ]; then
        echo "Push still failed. Surface the error to the user — do NOT delete the branch or worktree."
        exit 1
    fi
fi
echo "origin/master in sync — fix is shipped."
```

Only declare the /fix done after this step prints the success line. If the push fails (auth, no remote, etc.), report it to the user — leave the merged commit in place locally and stop.

### Abandoning a worktree

If the fix doesn't pan out and the user wants to throw it away instead of merging:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
git worktree remove "${WORKTREE_DIR}"   # or --force if td5re.exe is still running
git branch -D "${SESSION_TAG}"          # -D because it was never merged
```

Ask the user to confirm before running the `-D` variant.

**CRITICAL — junction safety:** Teardown must go through `git worktree remove`, never `rm -rf "${WORKTREE_DIR}"`. The worktree contains a junction at `td5mod/deps/mingw/` that points back into the main tree (753 MB MinGW toolchain). A recursive `rm` from git-bash or PowerShell `Remove-Item -Recurse` will follow that junction and **destroy the toolchain in the main tree**.

**`git worktree remove --force` is NOT safe with this junction.** Empirically confirmed on 2026-04-16 and again on 2026-04-20: when a worktree has uncommitted files (td5re.ini tweaks, log/, build artifacts) plain `git worktree remove` refuses, and the agent reaches for `--force` — which traverses the mingw junction and wipes main's toolchain. Before EVERY `git worktree remove --force`, pre-unlink the junction:

```bash
cmd //c "rmdir \"$(cygpath -w "${WORKTREE_DIR}")\\td5mod\\deps\\mingw\""   # no /s — removes only the junction
git worktree remove --force "${WORKTREE_DIR}"                              # safe now, junction is gone
```

As of 2026-04-20 main's `td5mod/deps/mingw/` has the READ-ONLY attribute set via `attrib +R /S /D` so a forgotten pre-unlink step fails with EACCES instead of silently wiping the toolchain. Do not clear that attribute.

**ALSO CRITICAL — never `rm` a junction inside the worktree, even ad-hoc.** This trap fires outside teardown too: if you see `${WORKTREE_DIR}/td5mod/deps/mingw` listed by MSYS as a `symlink` (because git-bash renders Windows junctions that way), DO NOT `rm -f` it to "clean it up before recreating". Bash's `rm` follows junctions into their target and empties the destination. The 2026-04-15 incident: agent ran `rm -f "${WORKTREE_DIR}/td5mod/deps/mingw"` to recreate a junction → emptied main's `td5mod/deps/mingw/` → had to re-extract `td5mod/deps/mingw-i686.7z` to restore. The `~/bin/rm` wrapper now refuses paths under main's `td5mod/deps/mingw`, but the wrapper resolves through junctions, so the worktree path is also blocked. To unlink a junction safely, use `cmd //c "rmdir <path>"` (no `/s`) — Windows `rmdir` removes the junction without recursing into it. Recovery if the toolchain is wiped: `cd td5mod/deps && rm -rf mingw && 7z x mingw-i686.7z -y && mv mingw32 mingw/`.

## Testing the build (INI + log loop)

After a successful build, **you can run the game yourself** to validate the fix — do not ask the user to launch it. The flow is: configure `td5re.ini` **inside the worktree**, run the auto-trace harness from the worktree, then read the worktree's log.

Because every `/fix` session runs in its own worktree, your INI tweaks and log output are guaranteed not to collide with other sessions.

### Configure the run via INI

`${WORKTREE_DIR}/td5re.ini` controls every test knob for this session. Edit it directly with the `Edit` tool before launching — **never edit the main tree's `td5re.ini` during a `/fix` run**, that belongs to whoever is working in the main tree.

**MANDATORY first step before every launch:** Read `${WORKTREE_DIR}/td5re.ini` and `Edit` in the following baseline flags — these are non-negotiable for `/fix` testing runs (see `feedback_fix_ini_defaults.md`):

| Section | Key | Value | Why |
|---|---|---|---|
| `[Game]` | `AutoRace` | `1` | Boot straight into the race, no menu input needed |
| `[Game]` | `SkipIntro` | `1` | Skip legal/intro FMVs |
| `[Game]` | `DebugOverlay` | `1` | **Always on** — on-screen counters must be visible every run |
| `[Game]` | `DefaultTrack` | `0` (Moscow) | Default unless `$ARGUMENTS` names another track — see below |
| `[GameOptions]` | `PlayerIsAI` | `0` | **Default off** — user drives manually. Only flip to `1` if the fix is specifically about attract-mode / AI-slot-0 / `/diff-race` behavior |
| `[Logging]` | `Enabled` | `1` | **Mandatory for `/fix`** — master `td5re.ini` default is `0` (perf baseline). Without this flip the post-run log reads in **Logging Rules** below see only `[session start]`. Leave `Wrapper = 0` unless you're actually debugging the D3D shim |

**Track selection rule:** Parse `$ARGUMENTS` for a track name. If it mentions one of the quickrace mapping entries (Moscow, Edinburgh, Sydney, Blue Ridge, Jarash, Newcastle, Maui, Courmayeur, Honolulu, Tokyo, Keswick, San Francisco, Bern, Kyoto, Washington, Munich, Cheddar, Montego, Bez, Drag Strip), look up its index in `re/tools/quickrace/td5_quickrace.ini` and set `DefaultTrack` accordingly. Otherwise use `0` (Moscow). Moscow is the default because it's the reference track used by `/diff-race` and every regression baseline.

**PlayerIsAI exception:** Do NOT auto-flip to `1`. The only cases where you should ever set `PlayerIsAI = 1` in `/fix` are when the user explicitly asks to test AI behavior on slot 0 (attract-mode parity, AI rubber-banding, script VM). Never for general physics / camera / HUD / render fixes.

The worktree INI is seeded from the committed master copy, which usually has these keys at the wrong defaults — so the full set of edits is needed on every fresh worktree. Do NOT launch the exe until all six keys are written to disk.

Useful keys for fix-validation runs:

| Section | Key | Effect |
|---|---|---|
| `[Game]` | `SkipIntro = 1` | Bypass legal/intro FMVs |
| `[Game]` | `AutoRace = 1` | Boot straight into a race (no menu input) — **disable for /diff-race** |
| `[Game]` | `DefaultTrack` / `DefaultCar` / `DefaultGameType` | Pick the scenario for AutoRace |
| `[Game]` | `DebugOverlay = 1` | On-screen counters for physics/render |
| `[GameOptions]` | `Laps`, `Traffic`, `Cops`, `Difficulty`, `Collisions`, `Dynamics` | Race-rules toggles |
| `[GameOptions]` | `PlayerIsAI = 1` | Slot 0 driven by AI (mirrors original demo mode) — **disable for /diff-race** so player-path fixes are visible |
| `[Trace]` | `RaceTrace = 1` | Emit `log/race_trace.csv` |
| `[Trace]` | `RaceTraceSlot` | Which actor slot to record |
| `[Trace]` | `RaceTraceMaxFrames` | Cap trace length |
| `[Trace]` | `AutoThrottle = 1` | Hold throttle automatically once the race starts |
| `[Logging]` | `Enabled = 1` | **Required** — turn on the centralized logger so `engine.log` / `race.log` / `frontend.log` populate (otherwise post-run reads see only `[session start]`) |
| `[Logging]` | `MinLevel` | `0`=DEBUG, `1`=INFO (default), `2`=WARN, `3`=ERROR. Bump to `2` to keep WRN/ERR but kill the INF spam |
| `[Logging]` | `Frontend` / `Race` / `Engine` / `Wrapper` | Per-sink gates. Turn off the categories you didn't touch to keep the log lean. `Wrapper` is the biggest emitter — leave at `0` unless debugging the D3D shim |

For deeper scenarios (specific track + car + game type without touching `td5re.ini`), the shared launcher INI at `re/tools/quickrace/td5_quickrace.ini` overlays on top — see `[reference_quickrace_launcher.md]` for the full track/car tables. **Reminder:** the shared overlay runs *after* `td5re.ini` and silently overwrites overlapping keys.

### Launch the game (auto-close harness)

**Always drive the race via `AutoRace = 1` + `SkipIntro = 1`. Never use `SendKeys F5` — it's unreliable, and the AutoRace path is the supported harness.** Set those two flags in `${WORKTREE_DIR}/td5re.ini` before launching.

Launch from the worktree so it picks up the worktree's `td5re.exe`, INI, and log dir — NOT the main tree. **Set `TD5RE_WINDOW_TITLE` before launching** so the running window identifies which `/fix` candidate is on screen (critical when several worktrees are running side-by-side). Compose the title from the session tag and a compact version of `$ARGUMENTS`:

```bash
# Build a short, descriptive title — e.g. "TD5RE [fix-1712956800-42831] brake gate on reverse"
# Trim $ARGUMENTS to ~80 chars and strip newlines so the title bar stays readable.
FIX_DESC="$(printf '%s' "$ARGUMENTS" | tr '\n' ' ' | cut -c1-80)"
WIN_TITLE="TD5RE [${SESSION_TAG}] ${FIX_DESC}"

cd "${WORKTREE_DIR}" && TD5RE_WINDOW_TITLE="${WIN_TITLE}" powershell.exe -Command "
\$env:TD5RE_WINDOW_TITLE = '${WIN_TITLE}'
Start-Process -FilePath '.\td5re.exe' -PassThru | ForEach-Object {
    \$proc = \$_
    Start-Sleep -Seconds 10
    \$proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 2
    if (!\$proc.HasExited) { \$proc.Kill() }
    Write-Host 'Done - process closed'
}
" 2>&1
```

If `${WIN_TITLE}` contains single quotes, escape them for the PowerShell string (double them up: `'` → `''`). Keep the title under ~180 chars — Windows truncates beyond that anyway.

Tune the inner `Start-Sleep` to your scenario — ~10s for grid/countdown/spawn/early-drive checks; **60+s** for slope/ramp/jump features further into the track (the first ~10s is always grid + countdown).

### Runtime asset + toolchain setup (first build inside a fresh worktree)

A brand-new worktree is missing gitignored runtime bits that the build and the exe need. Set these up once, right after `git worktree add` in Step 0.

**Design rule (2026-04-15): worktrees must be self-contained.** Only the MinGW toolchain is junctioned (read-only, 753 MB, nothing in the workflow tries to mutate it). `original/` and `re/assets/` runtime subdirs are **copied**, not junctioned — a careless `rm -rf` or bad merge inside a worktree can no longer reach main's irreplaceable game data. Known-good snapshot of both lives at `C:/Users/maria/Desktop/TD5RE_backup_2026-04-15/` for disaster recovery.

```bash
# Run from inside the worktree so `re\assets` / `original` resolve correctly
# and so the Windows-native robocopy DST paths stay simple (no cygpath nesting,
# which has silently dropped commands in the past).
cd "${WORKTREE_DIR}"

# 1. MinGW toolchain (gitignored, 753 MB, read-only — junction is safe).
#    Use a plain relative DST; absolute paths with embedded forward slashes
#    break mklink's "volume label syntax" parser.
cmd //c 'mklink /J td5mod\deps\mingw C:\Users\maria\Desktop\Proyectos\TD5RE\td5mod\deps\mingw'
test -d td5mod/deps/mingw/mingw32/bin || {
    echo "MINGW JUNCTION FAILED: td5mod/deps/mingw is not reachable. Aborting."
    exit 1
}

# 2. Original game files — COPY, not junction (~113 MB, ~15s).
#    Previous recipe junctioned main's original/ into the worktree; a recursive
#    delete or bad merge then followed the junction and destroyed main's only
#    copy of TD5_d3d.exe. Copying makes the worktree self-contained so teardown
#    can never reach main.
cmd //c 'robocopy C:\Users\maria\Desktop\Proyectos\TD5RE\original original /E /NFL /NDL /NJH /NJS /NC /NS /NP /MT:8' >/dev/null 2>&1
test -f original/TD5_d3d.exe || {
    echo "ORIGINAL COPY FAILED: original/TD5_d3d.exe missing. Aborting."
    exit 1
}

# 3. Extracted game assets — SINGLE robocopy over the whole re/assets/ tree,
#    using /XC /XN /XO to skip any file that already exists in the worktree.
#    This preserves branch-tracked entries (re/assets/static/*.dat,
#    manifest.json, catalog.json if present) without needing a per-entry
#    "skip if dest exists" loop. Per-entry loops with nested cmd //c +
#    cygpath quoting have silently dropped copies on this host; one flat
#    robocopy invocation is the only shape that reliably copies every subdir.
#    ~319 MB main total, ~300 MB net after skipping tracked entries, ~30s.
cmd //c 'robocopy C:\Users\maria\Desktop\Proyectos\TD5RE\re\assets re\assets /E /XC /XN /XO /NFL /NDL /NJH /NJS /NC /NS /NP /MT:8' >/dev/null 2>&1

# MANDATORY verification — robocopy can exit cleanly while copying nothing
# (e.g. locked target, blocked path, a stray junction). Compare file counts
# and abort loudly if the worktree is short, so the loop can't silently
# boot a game that fails to load tracks/cars.
MAIN_ASSET_COUNT=$(find C:/Users/maria/Desktop/Proyectos/TD5RE/re/assets -type f | wc -l)
WT_ASSET_COUNT=$(find re/assets -type f | wc -l)
echo "re/assets file count — main: ${MAIN_ASSET_COUNT}, worktree: ${WT_ASSET_COUNT}"
if [ "${WT_ASSET_COUNT}" -lt "${MAIN_ASSET_COUNT}" ]; then
    echo "ASSET COPY INCOMPLETE. Expected >=${MAIN_ASSET_COUNT}, got ${WT_ASSET_COUNT}."
    echo "Missing subdirs:"
    for d in cars cup environs extras frontend legals levels loading mugshots sound sounds static tracks traffic; do
        [ -e "C:/Users/maria/Desktop/Proyectos/TD5RE/re/assets/${d}" ] || continue
        if [ ! -e "re/assets/${d}" ]; then
            echo "  re/assets/${d}: MISSING"
        else
            mc=$(find "C:/Users/maria/Desktop/Proyectos/TD5RE/re/assets/${d}" -type f | wc -l)
            wc=$(find "re/assets/${d}" -type f | wc -l)
            [ "${wc}" -lt "${mc}" ] && echo "  re/assets/${d}: ${wc}/${mc}"
        fi
    done
    echo "Re-run the robocopy above manually from inside the worktree and inspect the output."
    exit 1
fi

# Sanity check: confirm nothing inside the worktree is a junction back to main
# (a worktree-level junction would let `rm -rf` in the worktree follow into
# main and destroy irreplaceable game data).
cmd //c 'dir /AL /S original re\assets' 2>&1 | grep -i "JUNCTION\|SYMLINK" && {
    echo "JUNCTION FOUND inside worktree — teardown could destroy main. Aborting."
    exit 1
} || echo "Junction check: clean"

# 4. Pre-built ddraw_wrapper (fresh wrapper build from inside the worktree is flaky on
#    Windows due to the shader pushd/popd sequence leaving cwd in a bad state). Just
#    reuse the main tree's build output unless the wrapper source actually changed.
mkdir -p td5mod/ddraw_wrapper/build
cp C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/ddraw_wrapper/build/*.o \
   C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/ddraw_wrapper/build/libddraw_wrapper.a \
   td5mod/ddraw_wrapper/build/
```

**Why a single robocopy, not a per-entry loop:** On this host, `cmd //c "robocopy \"$(cygpath -w "${entry}")\" ..."` inside a `for` loop silently drops most of its invocations — only the first one or two actually run. A single top-level robocopy with `/E /XC /XN /XO` copies the whole tree in one shot and respects branch-tracked files by skipping any file that already exists (Excluding Changed/Newer/Older together means "skip if present, regardless of timestamp"). If you ever need to re-sync, re-run the same command — it's idempotent.

Also sync any **uncommitted** `.c/.h` changes from the main tree into the worktree — `git worktree add master` only seeds committed state, so any files that are modified-but-unstaged or completely untracked (e.g. `td5mod/src/td5re/td5_trace.c/h`) will be missing. Before building:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE
git status --short td5mod/src/td5re/ td5mod/ddraw_wrapper/src/ | awk '/^ M|^\?\?/ {print $2}' | while read f; do
    mkdir -p "$(dirname "${WORKTREE_DIR}/${f}")"
    cp "${f}" "${WORKTREE_DIR}/${f}"
done
# Also mirror any files the main tree shows as deleted (D) so stale copies in the
# worktree don't sneak into the build.
git status --short td5mod/src/td5re/ | awk '/^ D/ {print $2}' | while read f; do
    rm -f "${WORKTREE_DIR}/${f}"
done
```

This keeps the worktree's runtime state bit-for-bit identical to what the user sees in the main tree.

### Read the log to verify the fix

After the harness exits, read the relevant log under `log/` (see the table in **Logging Rules** below). Match your edits to the right log file, read the tail (last 100–200 lines), and grep for the `TD5_LOG_I` lines you added. If the log contradicts the expected behavior, iterate — do NOT guess at runtime state when the log can tell you. For physics/AI fixes, also inspect `log/race_trace.csv` directly.

## Multi-track CSV bundle sweep (mandatory)

**Why:** A single-track test misses symptoms that only show up on other geometry. A Ghidra-correct fix can look "no effect" on Moscow because Moscow's spawn spans never hit the corrected code path. Multi-track coverage catches this; per-fix co-located CSV bundles make post-hoc bisection possible. See `feedback_fix_multi_track_frida.md` for the reasoning.

**When to run:** After the single-track runtime probe looks sane, BEFORE the Step 3 commit. Mandatory for any fix touching `td5_physics.c`, `td5_ai.c`, `td5_track.c`, `td5_camera.c`, `td5_game.c`, `td5_vfx.c`, or `td5_hud.c`. Skip only when the fix is pure frontend / asset / build-tooling.

### The trio

| Slot | Track | Index | Why |
|------|-------|-------|-----|
| 1 | Moscow | `0` | Point-to-point; regression baseline, matches /diff-race default |
| 2 | Newcastle | `5` | Circuit; exercises lap-wrap, checkpoint array, circuit AI branch |
| 3 | Random | pick from `{1,2,3,4,6,7,8,9,10,11,12,13,14,15,16,17,18}` | Widens coverage over time; seed from `SESSION_TAG` so parallel sessions don't collide |

**Seeding the random pick:**
```bash
RANDOM_POOL=(1 2 3 4 6 7 8 9 10 11 12 13 14 15 16 17 18)
SEED=$(( $(echo "${SESSION_TAG}" | cksum | awk '{print $1}') ))
RANDOM_IDX=${RANDOM_POOL[$(( SEED % ${#RANDOM_POOL[@]} ))]}
# Also capture a human-readable name for the bundle filename:
declare -A TRACK_NAMES=(
    [0]=moscow [1]=edinburgh [2]=sydney [3]=blueridge [4]=jarash [5]=newcastle
    [6]=maui [7]=courmayeur [8]=honolulu [9]=tokyo [10]=keswick [11]=sanfrancisco
    [12]=bern [13]=kyoto [14]=washington [15]=munich [16]=cheddar [17]=montego
    [18]=bez [19]=dragstrip
)
RANDOM_NAME="${TRACK_NAMES[${RANDOM_IDX}]}"
```

### Bundle layout

Bundles live in the MAIN tree (so they survive worktree teardown). Each `/fix` session writes exactly one bundle dir named after its session tag:

```
tools/frida_csv/
    fix-1776736681-282344/
        original_track0_moscow.csv       # Frida hook on TD5_d3d.exe
        fix_track0_moscow.csv            # port's log/race_trace.csv
        original_track5_newcastle.csv
        fix_track5_newcastle.csv
        original_track<N>_<name>.csv     # the random pick
        fix_track<N>_<name>.csv
        meta.json                        # session tag, branch, commit, trio, timestamps
```

### Sweep procedure

Loop over the trio. For each iteration, `TRACK_N` is the track index and `TRACK_NAME` is the slug from the `TRACK_NAMES` map above (e.g. `moscow`, `newcastle`, `munich`).

```bash
BUNDLE_DIR="C:/Users/maria/Desktop/Proyectos/TD5RE/tools/frida_csv/${SESSION_TAG}"
mkdir -p "${BUNDLE_DIR}"

for entry in "0:moscow" "5:newcastle" "${RANDOM_IDX}:${RANDOM_NAME}"; do
    TRACK_N="${entry%%:*}"
    TRACK_NAME="${entry##*:}"
    echo "=== Sweep track=${TRACK_N} (${TRACK_NAME}) ==="

    # --- Port side: run the worktree's td5re.exe with DefaultTrack=N ---
    # Use the Edit tool to set [Game] DefaultTrack=${TRACK_N} in the
    # worktree INI; keep AutoRace=1, AutoThrottle=1, RaceTrace=1,
    # RaceTraceSlot=-1, RaceTraceMaxFrames=300 from the baseline config.
    cd "${WORKTREE_DIR}"
    rm -f log/race_trace.csv
    TD5RE_WINDOW_TITLE="TD5RE [${SESSION_TAG}] ${TRACK_NAME}_track${TRACK_N}" powershell.exe -Command "
    Start-Process -FilePath '.\td5re.exe' -PassThru | ForEach-Object {
        \$proc = \$_
        Start-Sleep -Seconds 20   # 20s covers countdown + ~14s of race
        \$proc.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 2
        if (!\$proc.HasExited) { \$proc.Kill() }
    }"
    cp log/race_trace.csv "${BUNDLE_DIR}/fix_track${TRACK_N}_${TRACK_NAME}.csv"

    # --- Original side: run TD5_d3d.exe under Frida from main ---
    cd C:/Users/maria/Desktop/Proyectos/TD5RE
    rm -f log/race_trace_original.csv
    python re/tools/quickrace/td5_quickrace.py --trace --trace-auto-exit \
        --set race.track=${TRACK_N} --set race.car=0 --set race.game_type=0 --set race.laps=1 \
        --trace-max-frames 300
    cp log/race_trace_original.csv "${BUNDLE_DIR}/original_track${TRACK_N}_${TRACK_NAME}.csv"

    # --- Sanity check: both sides must produce a non-empty CSV ---
    PORT_ROWS=$(wc -l < "${BUNDLE_DIR}/fix_track${TRACK_N}_${TRACK_NAME}.csv")
    ORIG_ROWS=$(wc -l < "${BUNDLE_DIR}/original_track${TRACK_N}_${TRACK_NAME}.csv")
    echo "track=${TRACK_N} (${TRACK_NAME}): port=${PORT_ROWS} rows, original=${ORIG_ROWS} rows"
    if [ "${PORT_ROWS}" -lt 50 ] || [ "${ORIG_ROWS}" -lt 50 ]; then
        echo "WARN: short CSV on track=${TRACK_N}; inspect worktree log/engine.log for cause"
    fi
done
```

The quickrace Python + `tools/frida_race_trace.js` already emit the canonical CSV schema that pairs 1:1 with the port's `log/race_trace.csv`. Do NOT reimplement the Frida side inline.

3. **Confirm both CSVs are non-empty** — ≥ 50 rows each. A zero-row side means the harness died before the trace flushed; investigate (missing windowed patch, DDraw proxy unrenamed, Frida attach race) and retry that track.

### Write `meta.json`

```bash
cat > "${BUNDLE_DIR}/meta.json" <<EOF
{
    "session_tag": "${SESSION_TAG}",
    "branch": "${SESSION_TAG}",
    "worktree": "${WORKTREE_DIR}",
    "fix_description": "<one-line from $ARGUMENTS>",
    "trio": [
        {"index": 0, "name": "moscow", "kind": "point-to-point"},
        {"index": 5, "name": "newcastle", "kind": "circuit"},
        {"index": ${RANDOM_IDX}, "name": "${RANDOM_NAME}", "kind": "random"}
    ],
    "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "card": {"car": 0, "game_type": 0, "laps": 1, "frames": 300}
}
EOF
```

### Prune to 10 newest bundles

After writing the new bundle, prune older `fix-*` dirs under `tools/frida_csv/` so only the 10 newest survive (by directory mtime):

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE/tools/frida_csv
# ls -dt sorts by mtime descending; keep first 10, delete the rest.
ls -dt fix-*/ 2>/dev/null | tail -n +11 | while read dir; do
    echo "Pruning old bundle: ${dir}"
    rm -rf "${dir%/}"
done
```

### Do NOT run `/diff-race` inline

The sweep's job is to produce the CSV bundle, not to diff it. Leave interpretation to `/diff-race` (which knows how to consume the bundle layout). Report to the user that the bundle is ready at `tools/frida_csv/${SESSION_TAG}/` and recommend `/diff-race bundle=${SESSION_TAG}` as the next step if they want a comparator pass.

### Troubleshooting the sweep

- **Port CSV empty on some tracks** — the worktree's td5re.exe might crash on specific tracks (e.g. spin-out locks the car in place and `simulation_tick_counter` never advances). Capture whatever rows did flush (the CSV is written incrementally); a short CSV is still a data point.
- **Original CSV empty** — common causes: `original/DDraw.dll` not renamed to `DDraw.dll.td5re_wrapper`, windowed-mode patch not applied, Frida attach race. See `/diff-race`'s Step 1 notes for the full checklist.
- **Random picks colliding across parallel `/fix` sessions** — SESSION_TAG includes the PID (`$$`), so two simultaneous runs always hash to different seeds. If you see two bundles with the same random index anyway, that's fine — both still cover Moscow + Newcastle distinctly.

## Logging Rules

Every module has a `LOG_TAG` constant and uses `TD5_LOG_I` / `TD5_LOG_W` / `TD5_LOG_E` macros from `td5_platform.h`. Logs are routed to separate files under `log/` based on module tag:

| Log file | Module tags |
|----------|-------------|
| `log/frontend.log` | `frontend`, `hud`, `save`, `input` |
| `log/race.log` | `td5_game`, `physics`, `ai`, `track`, `camera`, `vfx` |
| `log/engine.log` | `render`, `asset`, `platform`, `sound`, `net`, `fmv`, `main` |

### Auto-add logging to changed code

When implementing a fix, **always** add `TD5_LOG_I(LOG_TAG, ...)` calls at key decision points in the code you add or modify. This is mandatory — do not skip it. Examples of what to log:

- Entry to a newly added or rewritten block: `TD5_LOG_I(LOG_TAG, "func_name: description param=%d", val);`
- Conditional branches that affect behavior: log which branch was taken and why
- Values computed from RE constants that could be wrong: log the computed value

Keep log messages concise (one line, no multi-line formatting). Use `TD5_LOG_W` for fallback/unexpected paths, `TD5_LOG_E` for errors. Do not add logging inside per-frame tight loops (physics tick, render dispatch) — only at session/state-level events.

### Read logs when iterating

After a test run (user launches the game and reports a problem, or you are asked to iterate), **always read the relevant log file** before making further changes:

```
# Determine which log to read based on the modules you changed.
# Read from the WORKTREE, not the main tree.
Read ${WORKTREE_DIR}/log/frontend.log   # if you touched frontend/hud/save/input
Read ${WORKTREE_DIR}/log/race.log       # if you touched game/physics/ai/track/camera/vfx
Read ${WORKTREE_DIR}/log/engine.log     # if you touched render/asset/platform/sound/net/fmv
```

Read the log **tail** (last 100-200 lines) to see what happened at runtime. Use the log output to inform your next iteration — don't guess at runtime behavior when the logs can tell you.

## Key Rules

- The Ghidra research agent does ALL MCP work. Main context never sees raw decompilation output.
- Code edits, builds, and test runs happen inside the Step-0 worktree at `${WORKTREE_DIR}` — never in the main tree. The worktree is the unit of isolation between parallel `/fix` sessions.
- Do NOT merge to master or push until the user explicitly approves (Step 4). Worktrees are meant to coexist and be picked between.
- If the research agent can't find the relevant function, ask the user for hints (function name, address, or module) before retrying.
- If build fails after edits, try fixing compile errors directly (up to 2 attempts), then escalate to user.
- If the user returns to a session later and `${WORKTREE_DIR}` / `${SESSION_TAG}` are no longer in scope, re-derive them with `git worktree list` and pick the branch matching this session's fix.
