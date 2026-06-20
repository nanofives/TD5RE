# TD5RE Fix Workflow

Fix a bug or implement a feature in the TD5RE source port. Delegates Ghidra RE research to a subagent (keeps large MCP responses out of main context), then implements the fix **inside an isolated git worktree** so parallel `/fix` sessions can't clobber each other's `td5re.exe`, `td5re.ini`, or `log/`. On user approval the worktree is merged back and deleted.

**Usage:** `/fix <description of the bug or feature>`

## Workflow

### Step 0: Spin up an isolated test worktree

Every `/fix` run gets its own git worktree under `.claude/worktrees/` so builds, logs, and INI tweaks stay isolated from the main tree and from other concurrent sessions.

Pick a short unique session tag and create the worktree. By default branch from `master`, but allow opt-in to branch from the current HEAD when the user is mid-investigation on a topic branch with uncommitted work that the fix should build on. Run these from the main tree (`C:/Users/maria/Desktop/Proyectos/TD5RE`):

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Unique tag (epoch + PID + $RANDOM). Keep it short — also becomes the branch name.
# $RANDOM avoids collisions when two parallel bash shells fire in the same second
# with PID reuse (rare on Windows but seen empirically).
SESSION_TAG="fix-$(date +%s)-$$-${RANDOM}"
WORKTREE_DIR="C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/${SESSION_TAG}"

# --- Pick the source branch (QOL: don't silently lose in-progress work) ---
# Default is master. If the user is on a topic branch with uncommitted work,
# STOP and ask before branching from master — they may want to continue the
# investigation on top of their current HEAD instead of forking off master.
BASE_BRANCH="${BASE_BRANCH:-master}"
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
DIRTY_FILES="$(git status --porcelain | wc -l)"

if [ "${BASE_BRANCH}" = "master" ] && [ "${CURRENT_BRANCH}" != "master" ] && [ "${DIRTY_FILES}" -gt 0 ]; then
    echo "WARN: current branch is ${CURRENT_BRANCH} with ${DIRTY_FILES} uncommitted file(s)."
    echo "      Default /fix behavior branches from master and IGNORES your in-progress work."
    echo "      Ask the user which base they want before proceeding:"
    echo "        (a) master              — clean fork (default, recommended for unrelated fixes)"
    echo "        (b) ${CURRENT_BRANCH}   — continue investigation; uncommitted files mirrored at Step 0"
    echo "      Set BASE_BRANCH=${CURRENT_BRANCH} and re-run if they pick (b)."
    # If running inside an autonomous /fix where you cannot prompt, default to (a)
    # and call this out explicitly in the final report.
fi

# Create a new branch from BASE_BRANCH in its own worktree.
git worktree add -b "${SESSION_TAG}" "${WORKTREE_DIR}" "${BASE_BRANCH}"

# All subsequent edits / builds / runs happen inside $WORKTREE_DIR.
echo "Worktree ready at: ${WORKTREE_DIR}"
echo "Branch: ${SESSION_TAG} (forked from ${BASE_BRANCH})"
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

### Step 1.75: Localize via function-call trace (when prior `/diff-race` showed divergence)

If the bug was surfaced by a `/diff-race` run that flagged a tick-N divergence in pose / motion / track / yaw / spin, **don't skip to coding** — first localize WHICH function call inside tick N produces the bad state. This is the difference between "the chassis spins" (symptom) and "`update_susp_response` returns wrong ang_vel_roll given matching wheel-contact args" (mechanism).

**Workflow:**

1. From the `/diff-race` output, identify the per-actor field that diverges first (often `ang_yaw`, `ang_roll`, `vel_x`, `world_x`). The dominant Δ at the earliest sim_tick names the upstream subsystem:

   | Earliest divergent field | Likely upstream function | Original RVA |
   |--------------------------|--------------------------|--------------|
   | `world_x` / `world_z` at tick 0 or 1 | `InitializeActorTrackPose` | `0x00434350` |
   | `ang_roll` / `ang_pitch` settling at tick 1 | `UpdateVehicleSuspensionResponse` | `0x004057F0` |
   | `wheel_mask` / surface flags | `RefreshVehicleWheelContactFrames` | `0x00403720` |
   | `vel_x` / `long_speed` | `UpdatePlayerVehiclePhysics` | `0x00404760` |
   | `track_span_*` / `sub_lane` | `UpdateActorTrackPosition` | `0x004440F0` |
   | `ang_yaw` (no roll/pitch divergence) | AI command path → `UpdateActorTrackBehavior` | `0x00434FE0` |
   | finish/checkpoint/race_pos | `UpdateRaceOrder` / `UpdateRaceActors` | `0x0042F5B0` / `0x00436A70` |

2. Pick a YAML under `re/trace-hooks/` that covers the suspect chain (e.g. `tick1_spawn_chain.yaml` for tick-1 pose+susp+wheel chain). If none fits, write one — see `re/trace-hooks/README.md` for the spec format. **Always include 2-3 hooks**: one upstream, one mid, one downstream. A single hook at the end of the chain only tells you "the result is wrong"; the upstream hook tells you whether the inputs to that result were already wrong.

3. Add `TD5_TRACE_CALL_ENTER` and `TD5_TRACE_CALL_RET` macros to the port-side equivalents (whatever is named in `port_symbol`). These are zero-cost when tracing is disabled. Keep the macros in only as long as the investigation runs — strip before commit (search the diff for `TD5_TRACE_CALL_` and remove unless gated by `#ifdef TD5_TRACE_CALLS`).

4. Run `/diff-race --track <N> hooks=re/trace-hooks/<spec>.yaml profile=<lens>`. The orchestrator runs a second comparator pass on `log/calls_trace_original.csv` vs `log/calls_trace.csv` keyed by `(sim_tick, fn_name, call_idx)`.

5. Read the calls-trace diff:
   - **Upstream hook args match, downstream result diverges** → bug is inside the downstream function. Decompile that one function in Ghidra and audit against port.
   - **Upstream hook args already differ** → bug is FURTHER upstream than this YAML covers. Add another hook one level up (e.g. if `update_susp_response`'s actor pointer has wrong roll-state on entry, hook `refresh_wheel_contacts` and the function above it).
   - **Call counts per tick differ** → loop bound or early-out condition mismatch. Often a flag-comparison sign or off-by-one in a guard.

6. Treat this as the "ground truth" for Step 2. Do NOT start coding until the calls-trace diff isolates the divergence to a specific (function, tick, slot, arg-or-retval) tuple. Without this, /fix has historically chased symptoms — patching one observable while the upstream root cause produced two more.

**Always-trace rule:** If any user request mentions a runtime symptom (yaw spin, chassis launch, wrong race position, AI overshoot, finish missed) AND `/diff-race` shows divergence in the relevant module, this step is **mandatory before Step 2**. Skipping it on the assumption you "already know" what's wrong is the most common cause of failed fixes.

**Worktree gotcha:** `TD5_TRACE_CALL_ENTER`/`RET` macros must be added to the port copy in `${WORKTREE_DIR}/td5mod/src/td5re/`, not the main tree. Verify before building or you'll trace the wrong binary.

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
6. Report the result to the user: what was found, what was changed, build status, the worktree path so they can inspect or manually launch, AND the bundle path at `tools/frida_csv/${SESSION_TAG}/`. **If the fix can only be confirmed by the user driving/playing** (handling feel, brakes, analog throttle, AI/catchup feel, multiplayer with real controllers, subjective audio/visual quality), do NOT present it as confirmed — explicitly say a manual test is needed and follow the ask-first flow in [When the fix needs the user to test](#when-the-fix-needs-the-user-to-test-ask-first): finish all self-verification, prep `manual_drive.exe`, then ask and wait until they're ready.

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

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

Rules for this step:
- Use `git add` with explicit file paths, never `git add .` or `git add -A`.
- The commit subject must start with `fix:` and match the original `$ARGUMENTS` description.
- The body must cite the RE basis AND the bundle path from the multi-track sweep so a future bisect can reproduce the runs.
- **Do NOT push and do NOT merge to master yet.** The whole point of the worktree is that multiple candidate fixes can coexist unmerged.
- Report to the user: the worktree path, the branch name, what was changed, how to run it (`${WORKTREE_DIR}/td5re.exe`), AND the bundle dir. End with an explicit prompt: *"Merge this into master and delete the worktree? (yes / keep for more testing)"*.

### Step 4: Merge into master + delete worktree (only on user approval)

**Wait for an explicit "yes / merge / ship it" from the user.** Do NOT run this step automatically after Step 3. The user typically iterates across several worktrees before deciding which one is the keeper.

This is the **same single-shot robust flow as `/end` Steps 5–6** — merge under a lock, build-once-deploy-by-copy, push with reject-retry, then immediate junction-safe teardown. Resolve any conflicts **in this session** (don't punt them to the user). For the full rationale (merge lock, build-once-copy, junction safeguards) see `end.md`; the executable version is reproduced here so a mid-`/fix` approval can ship without invoking `/end` separately.

When the user approves:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# --- 1. Acquire the merge lock (serialize against parallel-session /end + /fix). ---
LOCK="C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/.merge.lock"
ACQUIRED=0
for attempt in $(seq 1 40); do
    if mkdir "${LOCK}" 2>/dev/null; then
        printf 'tag=%s pid=%s ts=%s\n' "${SESSION_TAG}" "$$" "$(date +%s)" > "${LOCK}/owner"; ACQUIRED=1; break
    fi
    if [ -f "${LOCK}/owner" ]; then
        LOCK_TS="$(awk -F= '/ts=/{print $NF}' "${LOCK}/owner" 2>/dev/null | tr -dc 0-9)"
        [ -n "${LOCK_TS}" ] && [ $(( $(date +%s) - LOCK_TS )) -gt 900 ] && { echo "Reclaiming stale lock"; rm -rf "${LOCK}"; continue; }
        echo "Another session holds the merge lock ($(cat "${LOCK}/owner")) — waiting…"
    fi
    ping -n 4 127.0.0.1 >/dev/null 2>&1 || true   # no-sleep ~3s wait (foreground sleep is blocked)
done
[ "${ACQUIRED}" -eq 1 ] || { echo "Could not acquire merge lock — retry shortly."; exit 1; }

# --- 2. Main tree must be clean (release lock + ask user if dirty from non-/fix work). ---
[ -n "$(git status --porcelain)" ] && { echo "Main tree dirty — commit/stash first."; rm -rf "${LOCK}"; exit 1; }

# --- 3. Re-sync if origin moved during the session (rebase branch + rebuild). ---
git fetch origin master --quiet
if [ "$(git rev-list --count master..origin/master)" -gt 0 ]; then
    git checkout master && git merge --ff-only origin/master
    git -C "${WORKTREE_DIR}" rebase origin/master   # conflicts → resolve in-session (see rules below)
    ( cd "${WORKTREE_DIR}/td5mod/src/td5re" && ./build_all.bat 2>&1 | tail -20 )
else
    git checkout master
fi

# --- 4. Forbidden-path guard + precondition. ---
BAD_DIFF="$(git diff --name-status master.."${SESSION_TAG}" \
    | awk '$2 ~ /^(original\/|re\/)/ && $2 !~ /^re\/(analysis|sessions|assets\/static\/.*\.dat)$/ {print}')"
[ -n "${BAD_DIFF}" ] && { echo "REFUSING TO MERGE — forbidden paths:"; echo "${BAD_DIFF}"; rm -rf "${LOCK}"; exit 1; }
test -d original && test -d re/assets || { echo "PRECONDITION FAILED: original/ or re/assets/ missing."; rm -rf "${LOCK}"; exit 1; }

# --- 5. Merge + post-guard. ---
git merge --no-ff "${SESSION_TAG}" -m "Merge ${SESSION_TAG}: <one-line description>"
if [ ! -d original ] || [ ! -d re/assets ]; then
    echo "POST-MERGE FAILURE: irreplaceable data gone. Reverting."; git reset --hard ORIG_HEAD; rm -rf "${LOCK}"; exit 1
fi

# --- 6. BUILD ONCE, DEPLOY BY COPY: the worktree already built the merged tree. ---
cp "${WORKTREE_DIR}/td5re.exe"         td5re.exe         2>/dev/null || echo "WARN: dev exe not copied"
cp "${WORKTREE_DIR}/td5re_release.exe" td5re_release.exe 2>/dev/null || echo "WARN: release exe not copied"

# --- 7. Push (one reject-retry) + verify. ---
if ! git push origin master 2>&1; then
    git fetch origin master --quiet
    git merge --ff-only origin/master || { echo "origin diverged — resolve manually."; rm -rf "${LOCK}"; exit 1; }
    git push origin master
fi
git fetch origin master --quiet
[ "$(git rev-list --count origin/master..master)" -gt 0 ] && { echo "Push pending — surface to user."; rm -rf "${LOCK}"; exit 1; }
echo "origin/master in sync."

# --- 8. Release lock (teardown only touches our own worktree). ---
rm -rf "${LOCK}"
```

**Then tear down immediately using the junction-safe block from [Abandoning a worktree](#abandoning-a-worktree) below** — pre-unlink the mingw junction (PowerShell reparse-delete), verify it's gone, `git worktree remove --force`, pre/post canary, `git branch -d`. Do NOT use the bare `git worktree remove` without the pre-unlink; `--force` follows the junction into main and wipes the toolchain.

Rules for this step:
- **Resolve conflicts in this session** (rebase in step 3 or the merge in step 5): read both sides, preserve both intents, `git add` + continue, build to verify. Escalate to the user ONLY for genuinely incompatible-intent conflicts. Never `-X ours/theirs` blind, never `git rebase --skip`. (Same protocol as `/end` Step 1.)
- If `git merge` reports `Your local changes would be overwritten` (main tree has uncommitted work), the clean-tree guard in step 2 already caught it — STOP and report; do NOT stash/discard.
- Teardown lock-handling: if `git worktree remove --force` fails because `td5re.exe`/a log is still open, name the holding process and retry; if still locked, the merge is DONE — leave the worktree registered for the next sweep, never half-remove it.
- Only `git branch -d` (rejects unmerged branches). Never `-D` here.
- **Push is mandatory** — not done until `git rev-list --count origin/master..master` returns `0`.

### Step 4.5: Propagate new master into sibling /fix worktrees (merge + re-populate)

After Step 4 merges the approved branch into master, every OTHER active `/fix` worktree is now one commit behind AND missing any new uncommitted-but-mirrored files / fresh assets that main has accumulated since the sibling was created. This step does **two** things per sibling: (a) merge new master, (b) re-populate uncommitted source mirrors + asset deltas so each sibling is byte-identical to "freshly created from current master" for its build inputs.

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

    # --- (a) Run the merge. --no-edit uses the default merge message; --no-ff
    # preserves the incoming history cleanly. Capture stderr so we can
    # pattern-match locks. ---
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

    # --- (b) Re-populate the sibling's working state from main so it's not
    # missing uncommitted .c/.h, new assets, or deps the user added in main
    # after this sibling was forked. This is the SAME logic as Step 0's
    # "Runtime asset + toolchain setup" but trimmed: junctions and pre-built
    # ddraw_wrapper objects don't need re-doing (they're already in place),
    # only the deltas matter. ---
    echo "    Re-populating uncommitted mirrors + asset delta into ${WT}..."

    # Mirror uncommitted .c/.h from main into the sibling so they don't build
    # against a stale source set. Modified, untracked, AND deleted (so stale
    # copies in the worktree don't sneak into the build).
    (
        cd C:/Users/maria/Desktop/Proyectos/TD5RE
        git status --short td5mod/src/td5re/ td5mod/ddraw_wrapper/src/ 2>/dev/null \
            | awk '/^ M|^\?\?/ {print $2}' | while read f; do
                mkdir -p "$(dirname "${WT}/${f}")"
                cp "${f}" "${WT}/${f}" 2>/dev/null || true
            done
        git status --short td5mod/src/td5re/ 2>/dev/null \
            | awk '/^ D/ {print $2}' | while read f; do
                rm -f "${WT}/${f}" 2>/dev/null || true
            done
    )

    # Asset delta — idempotent robocopy. /XC /XN /XO means "skip if file
    # already exists regardless of timestamp" so this only copies NEW files
    # added to main since the sibling was created. Cheap when nothing changed.
    cmd //c "robocopy C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\re\\assets \"$(cygpath -w "${WT}")\\re\\assets\" /E /XC /XN /XO /NFL /NDL /NJH /NJS /NC /NS /NP /MT:8" >/dev/null 2>&1 || true

    echo "    ${WT}: merged + re-populated"
done
```

Rules for this step:
- **Any lock → stop immediately, abort that worktree's partial merge, and report.** Do not move on to other worktrees — the user asked to retry manually once the running test closes.
- **Any conflict → stop and leave the conflict in place** for manual resolution. Do not `--abort` in the conflict case; the user wants to see the diff.
- Never pass `--force` or `-X theirs/ours`. If a merge needs strategy help, that's a user decision.
- Don't touch worktrees outside `.claude/worktrees/fix-*` (the main tree is already updated, other unrelated worktrees are not our business).
- The re-population step (b) ONLY runs after the merge succeeds for that sibling. If the merge failed (lock/conflict), the loop has already `break`'d and we never reach re-population — that's intentional so we don't paper over a broken merge state.
- If the loop breaks early due to a lock/conflict, later worktrees stay unmerged — that's fine. Running this step again after the blocker clears will pick them up (git skips worktrees that are already up to date, and the asset robocopy is idempotent).

### Step 4.6: Final Ghidra pool sync (republish master's RE annotations)

Step 1 (research agent) runs `ghidra_pool.sh cleanup` + `sync` at the end of its own work, but only inside the research subagent's scope. Steps 2–4 never touch the pool. If the shipped fix included any Ghidra annotations (function renames, struct edits, comments, retypes) the master `TD5.rep` now contains them — but pool slots still hold the pre-merge snapshot. Republish so the next `/fix` (or `/re`, or any concurrent session) sees up-to-date RE state.

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Clean any stale lock files first so locked slots that died ungracefully
# don't get skipped forever. The pool script already protects active slots
# (it skips ones with a live .lock file from another running session).
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" cleanup
bash "C:/Users/maria/Desktop/Proyectos/TD5RE/scripts/ghidra_pool.sh" sync
```

Rules for this step:
- This is **always** safe to re-run. Locked slots get logged-as-skipped, not failed. Idempotent.
- Failure here is not fatal — the fix is already merged and pushed. Log the error and continue to Step 5. (Locked pool slots will catch up on the next `/fix` or manual `ghidra_pool.sh sync`.)
- Do NOT run `ghidra_pool.sh init` here — that recreates ALL slots from scratch and wipes any in-progress edits in pool slots that another session is using.

### Step 5: Post-condition — verify origin is in sync (HARD STOP)

After Step 4.5 (sibling worktree propagation) and Step 4.6 (final pool sync), run a final check that local master is fully published. This catches any case where the push was skipped — e.g. fallback cherry-pick path (Step 4 rules), partial flow due to manual recovery, or auth failure that wasn't surfaced.

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

> ### 🛡️ TEARDOWN SAFEGUARDS (added 2026-05-28 after a data-loss incident)
>
> A worktree teardown that followed junctions into the main tree wiped main's
> `re/`, `original/`, `td5mod/deps/mingw/`, AND `ddraw_wrapper/build/`. Three
> protections are now in place — respect ALL three:
>
> 1. **OS-level deny-delete ACL** on `original/` and `td5mod/deps/mingw/`
>    (icacls DENY `(DE,DC)` for `MARIANO-PC\maria` + `MARIANO-PC\CodexSandboxUsers`,
>    `(OI)(CI)` inherited). A teardown/delete that reaches these now fails with
>    **Access Denied — that is the safeguard WORKING.** NEVER strip the ACL to
>    "make teardown succeed." If `git worktree remove` errors Access-Denied on a
>    main-tree path, a junction was still live → unlink it (step 3) and retry.
>    To *legitimately* replace original/mingw, drop the deny first then re-apply:
>    `powershell.exe -NoProfile -Command "icacls '<dir>' /remove:d 'MARIANO-PC\maria' /remove:d 'MARIANO-PC\CodexSandboxUsers'"`
>    (the READ-ONLY `attrib +R` mentioned later in this doc did NOT hold — the ACL supersedes it.)
>
> 2. **Off-disk backup** at `C:\Users\maria\Desktop\TD5RE_backup_2026-05-28\`
>    (`original/` + `mingw-i686.7z`). Recovery if main data vanishes:
>    `re/` tracked files → `git checkout HEAD -- re/`; `original/` + `re/assets`
>    → robocopy from the backup or a sibling worktree; `mingw` → re-extract
>    `td5mod/deps/mingw-i686.7z` (build zlib from source — winlibs omits it);
>    `ddraw_wrapper/build` → recompile `src/*.c` + `ar rcs libddraw_wrapper.a`.
>
> 3. **Correct junction unlink for THIS host:** `cmd //c "rmdir ..."` FAILS here
>    with a volume-label syntax error (confirmed 2026-05-28), silently leaving the
>    junction live for `--force` to follow. Use PowerShell's reparse-aware delete
>    (removes ONLY the link, never the target), with a ReparsePoint guard.
>
> **Pre/post canary — run around EVERY teardown:** before, confirm
> `original/TD5_d3d.exe` and `td5mod/deps/mingw/mingw32/bin/gcc.exe` exist; after,
> confirm they STILL exist. If either vanished, the teardown followed a junction —
> STOP and restore from the backup immediately.

If the fix doesn't pan out and the user wants to throw it away instead of merging:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# CANARY (pre): record that the irreplaceable main-tree markers exist.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "PRECONDITION: main-tree markers already missing — investigate before teardown"; exit 1; }

# Always pre-unlink the mingw junction before teardown — plain `git worktree
# remove` will refuse (dirty files), and --force follows the junction into main.
# THIS HOST: cmd //c "rmdir" fails (volume-label error) — use PowerShell reparse delete.
powershell.exe -NoProfile -Command "\$p='${WORKTREE_DIR}/td5mod/deps/mingw' -replace '/','\\'; if (Test-Path \$p){ \$i=Get-Item \$p -Force; if (\$i.Attributes -band [IO.FileAttributes]::ReparsePoint){ \$i.Delete(); 'unlinked' } else { Write-Error 'NOT a reparse point — refusing'; exit 1 } }"

# MANDATORY: abort if the junction is still present — do NOT proceed to --force.
if [ -d "${WORKTREE_DIR}/td5mod/deps/mingw" ]; then
    echo "ERROR: pre-unlink failed — junction still live. Do NOT run --force."
    exit 1
fi

git worktree remove --force "${WORKTREE_DIR}"
git branch -D "${SESSION_TAG}"          # -D because it was never merged
```

Ask the user to confirm before running the `-D` variant.

**CRITICAL — junction safety:** Teardown must go through `git worktree remove`, never `rm -rf "${WORKTREE_DIR}"`. The worktree contains a junction at `td5mod/deps/mingw/` that points back into the main tree (753 MB MinGW toolchain). A recursive `rm` from git-bash or PowerShell `Remove-Item -Recurse` will follow that junction and **destroy the toolchain in the main tree**.

**`git worktree remove --force` is NOT safe with this junction.** Empirically confirmed on 2026-04-16, 2026-04-20, and 2026-04-22: when a worktree has uncommitted files (td5re.ini tweaks, log/, build artifacts) plain `git worktree remove` refuses, and the agent reaches for `--force` — which traverses the mingw junction and wipes main's toolchain. Before EVERY `git worktree remove --force`, pre-unlink the junction:

```bash
# THIS HOST: cmd //c "rmdir" fails with a volume-label syntax error (confirmed
# 2026-05-28), silently leaving the junction live for --force to follow. Use the
# PowerShell reparse-aware delete (removes ONLY the link, never the target) —
# see the TEARDOWN SAFEGUARDS block above.
powershell.exe -NoProfile -Command "\$p='${WORKTREE_DIR}/td5mod/deps/mingw' -replace '/','\\'; if (Test-Path \$p){ \$i=Get-Item \$p -Force; if (\$i.Attributes -band [IO.FileAttributes]::ReparsePoint){ \$i.Delete() } else { Write-Error 'NOT a reparse point — refusing'; exit 1 } }"

# MANDATORY: verify the junction is gone before proceeding. If it still exists,
# the unlink failed — do NOT run --force.
if [ -d "${WORKTREE_DIR}/td5mod/deps/mingw" ]; then
    echo "ERROR: pre-unlink failed — ${WORKTREE_DIR}/td5mod/deps/mingw still exists."
    echo "STOP: do NOT run git worktree remove --force. Investigate the unlink failure."
    exit 1
fi

git worktree remove --force "${WORKTREE_DIR}"   # safe now — junction is gone

# CANARY (post): the deny-delete ACL on original//mingw should block any
# junction-follow, but verify the irreplaceable markers survived regardless.
test -f original/TD5_d3d.exe && test -f td5mod/deps/mingw/mingw32/bin/gcc.exe \
  || { echo "FATAL: main-tree data vanished during teardown — restore from C:\\Users\\maria\\Desktop\\TD5RE_backup_2026-05-28\\ NOW"; exit 1; }
```

**Stale-build janitor (2026-04-29).** After the worktree is gone, sweep
any orphan `build_<pid>/` dirs from main's `td5mod/src/td5re/`. These
are leftovers from an earlier /fix variant (or any path that ran
`./build_standalone.bat <numeric-arg>` from main). The canonical
`build/` is preserved; only purely-numeric `build_NNNN/` dirs are
removed. Named variants (e.g. `build_ttfonts/`) are left alone in case
they were created on purpose.

```bash
# Bounded sweep — main tree only, numeric-suffix only, NEVER recurses
# through junctions because we only target one fixed directory.
shopt -s nullglob
for d in /c/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/build_[0-9]*/; do
    # Sanity: name must be entirely build_<digits>; reject anything else
    base=$(basename "$d")
    if [[ "$base" =~ ^build_[0-9]+$ ]]; then
        rm -rf "$d"
    fi
done
shopt -u nullglob
```

This step is **idempotent**: if no orphans exist, it's a no-op. It
runs even when the worktree was clean — the cost is one `ls` worth of
work and the upside is bounded recovery (single /fix run created
~5 MB of orphans before this guard; the historical accumulation was
~6 GB by 2026-04-29).

**As of 2026-05-28, main's `td5mod/deps/mingw/` AND `original/` are protected by a deny-delete ACL** (icacls DENY `(DE,DC)` for `MARIANO-PC\maria` + `MARIANO-PC\CodexSandboxUsers`, `(OI)(CI)` inherited) — a junction-follow delete now fails with Access Denied instead of wiping the data. This SUPERSEDES the older `attrib +R /S /D` READ-ONLY approach, which did NOT hold (the toolchain was wiped anyway on 2026-05-28). Do not clear the deny ACL; to legitimately replace these dirs, drop the deny with `icacls '<dir>' /remove:d 'MARIANO-PC\maria' /remove:d 'MARIANO-PC\CodexSandboxUsers'` and re-apply afterward.

**ALSO CRITICAL — never `rm` a junction inside the worktree, even ad-hoc.** This trap fires outside teardown too: if you see `${WORKTREE_DIR}/td5mod/deps/mingw` listed by MSYS as a `symlink` (because git-bash renders Windows junctions that way), DO NOT `rm -f` it to "clean it up before recreating". Bash's `rm` follows junctions into their target and empties the destination. The 2026-04-15 incident: agent ran `rm -f "${WORKTREE_DIR}/td5mod/deps/mingw"` to recreate a junction → emptied main's `td5mod/deps/mingw/` → had to re-extract `td5mod/deps/mingw-i686.7z` to restore. The `~/bin/rm` wrapper now refuses paths under main's `td5mod/deps/mingw`, but the wrapper resolves through junctions, so the worktree path is also blocked. To unlink a junction safely, use `cmd //c "rmdir <path>"` (no `/s`) — Windows `rmdir` removes the junction without recursing into it. Recovery if the toolchain is wiped: `cd td5mod/deps && rm -rf mingw && 7z x mingw-i686.7z -y && mv mingw32 mingw/`.

### Worktree janitor (bulk cleanup of abandoned worktrees)

`/fix` creates a fresh worktree on every invocation. Over time `.claude/worktrees/` accumulates dozens-to-hundreds of leftovers: candidate fixes that weren't merged, agent subagent runs, half-finished sessions. By late 2026 this directory routinely reaches 15–30 GB. This recipe categorizes every worktree by what it actually contains and tears down the safely-disposable ones. It **also sweeps stray branch refs** — `worktree-agent-*` (subagent worktree-isolation leftovers) and orphaned `fix-*` branches whose worktree is already gone — which never show up in `git worktree list` and otherwise accumulate invisibly as dangling refs.

**This is destructive. NEVER run automatically — always behind an explicit user request like "clean up worktrees" or "run the worktree janitor".** Even then, run the dry-run pass first, show the user the categorized output, and wait for confirmation before deleting anything that holds unique commits. (`/end` runs a merged-only subset of this automatically at session close — see end.md Step 7.5.)

The janitor categorizes each worktree into one of four buckets:

| Bucket | Definition | Default action |
|---|---|---|
| `LOCKED` | `git worktree list` shows `locked` — usually means an active session holds it OR a `worktree.lock` file exists from a crashed session | Skip; report count |
| `MERGED` | Every commit on the worktree's branch is reachable from master | Safe to remove (no work loss) |
| `STALE` | Branch has zero commits beyond its fork point AND working tree has no uncommitted changes | Safe to remove (nothing was ever done in it) |
| `UNIQUE` | Branch carries commits NOT in master OR working tree has uncommitted changes | Needs user decision per worktree |

Algorithm:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Make sure master is up to date locally so "reachable from master" is accurate.
git fetch origin master --quiet 2>/dev/null || true

# Storage for the report. Use temp files so the bash loop's subshell scoping
# doesn't lose counts.
REPORT_DIR="$(mktemp -d)"
: > "${REPORT_DIR}/locked.txt"
: > "${REPORT_DIR}/merged.txt"
: > "${REPORT_DIR}/stale.txt"
: > "${REPORT_DIR}/unique.txt"

# Iterate the canonical list (machine-readable) instead of shelling out to ls.
# Format: each worktree is a blank-line-separated record with key/value lines.
git worktree list --porcelain | awk '
    /^worktree / { wt=$2 }
    /^branch /   { br=$2; sub(/^refs\/heads\//, "", br) }
    /^locked/    { locked=1 }
    /^detached/  { detached=1 }
    /^$/         {
        if (wt) printf "%s\t%s\t%d\t%d\n", wt, br, locked+0, detached+0
        wt=""; br=""; locked=0; detached=0
    }
    END {
        if (wt) printf "%s\t%s\t%d\t%d\n", wt, br, locked+0, detached+0
    }
' | while IFS=$'\t' read -r WT BR LOCKED DETACHED; do
    # The main tree itself shows up in the list — skip it.
    case "${WT}" in
        */C--Users-maria-Desktop-Proyectos-TD5RE) continue ;;
        C:/Users/maria/Desktop/Proyectos/TD5RE)  continue ;;
    esac
    # Only act on .claude/worktrees/ paths — never touch unrelated worktrees.
    case "${WT}" in
        */.claude/worktrees/*) ;;
        *) continue ;;
    esac

    # Locked worktrees are off-limits — they're either in active use or a
    # crashed session left a marker we can't safely override.
    if [ "${LOCKED}" -eq 1 ]; then
        echo "${WT}|${BR}" >> "${REPORT_DIR}/locked.txt"
        continue
    fi

    # Detached HEAD with no branch — treat as STALE only if the working tree
    # is clean. Otherwise UNIQUE (there's uncommitted state).
    if [ "${DETACHED}" -eq 1 ]; then
        DIRTY="$(git -C "${WT}" status --porcelain 2>/dev/null | wc -l)"
        if [ "${DIRTY}" -eq 0 ]; then
            echo "${WT}|<detached>" >> "${REPORT_DIR}/stale.txt"
        else
            echo "${WT}|<detached>|dirty=${DIRTY}" >> "${REPORT_DIR}/unique.txt"
        fi
        continue
    fi

    # Count commits on this branch NOT reachable from master.
    UNIQUE_COMMITS="$(git rev-list --count master.."${BR}" 2>/dev/null || echo "?")"
    DIRTY="$(git -C "${WT}" status --porcelain 2>/dev/null | wc -l)"

    if [ "${UNIQUE_COMMITS}" = "?" ]; then
        # Branch disappeared or refs broken — treat as UNIQUE so the user inspects.
        echo "${WT}|${BR}|refs-broken" >> "${REPORT_DIR}/unique.txt"
    elif [ "${UNIQUE_COMMITS}" -eq 0 ] && [ "${DIRTY}" -eq 0 ]; then
        # Branch is at-or-behind master with a clean tree. Distinguish:
        #   - If branch == master exactly → STALE (never did anything)
        #   - If branch has commits BEHIND master but none ahead → MERGED
        BEHIND="$(git rev-list --count "${BR}"..master 2>/dev/null || echo 0)"
        if [ "${BEHIND}" -eq 0 ]; then
            echo "${WT}|${BR}" >> "${REPORT_DIR}/stale.txt"
        else
            echo "${WT}|${BR}|behind=${BEHIND}" >> "${REPORT_DIR}/merged.txt"
        fi
    else
        echo "${WT}|${BR}|ahead=${UNIQUE_COMMITS}|dirty=${DIRTY}" >> "${REPORT_DIR}/unique.txt"
    fi
done

# --- Stray branch refs (no live worktree) ---------------------------------
# worktree-agent-* branches (subagent worktree-isolation leftovers) and any
# fix-* branch whose worktree is already gone never appear in `git worktree
# list`, so the loop above misses them entirely. Categorize them separately:
# merged into master = safe to delete (label-only); unmerged = needs a decision.
: > "${REPORT_DIR}/branches_merged.txt"
: > "${REPORT_DIR}/branches_unmerged.txt"
CHECKED_OUT="$(git worktree list --porcelain | awk '/^branch /{sub(/^refs\/heads\//,"",$2);print $2}')"
for B in $(git for-each-ref --format='%(refname:short)' \
            'refs/heads/worktree-agent-*' 'refs/heads/fix-*'); do
    [ "${B}" = "master" ] && continue
    printf '%s\n' "${CHECKED_OUT}" | grep -qx "${B}" && continue   # still has a worktree → categorized above
    if git merge-base --is-ancestor "${B}" master 2>/dev/null; then
        echo "${B}" >> "${REPORT_DIR}/branches_merged.txt"
    else
        AHEAD="$(git rev-list --count "master..${B}" 2>/dev/null || echo '?')"
        echo "${B}|ahead=${AHEAD}" >> "${REPORT_DIR}/branches_unmerged.txt"
    fi
done

# Print the categorized report.
echo "=== Worktree janitor report ==="
echo "LOCKED  (skip — in-use or crashed):  $(wc -l < "${REPORT_DIR}/locked.txt")"
echo "MERGED  (safe to remove):            $(wc -l < "${REPORT_DIR}/merged.txt")"
echo "STALE   (safe to remove):            $(wc -l < "${REPORT_DIR}/stale.txt")"
echo "UNIQUE  (needs user decision):       $(wc -l < "${REPORT_DIR}/unique.txt")"
echo "BRANCHES merged   (no worktree, safe to delete):  $(wc -l < "${REPORT_DIR}/branches_merged.txt")"
echo "BRANCHES unmerged (no worktree, needs decision):  $(wc -l < "${REPORT_DIR}/branches_unmerged.txt")"
echo ""
echo "Report files at ${REPORT_DIR}/{locked,merged,stale,unique,branches_merged,branches_unmerged}.txt"
```

**Report the counts to the user and STOP.** Show the contents of `unique.txt` (worktree, branch, ahead-count, dirty-count) and ask:

```
Janitor found:
  - <N> MERGED worktrees          (branch fully in master, safe to remove)
  - <N> STALE worktrees           (branch is at-or-behind master, clean tree, safe to remove)
  - <N> UNIQUE worktrees          (each has commits not in master OR uncommitted changes)
  - <N> LOCKED worktrees          (skipped — in-use or crashed)
  - <N> merged stray branches     (no worktree, fully in master — label-only, safe to delete)
  - <N> unmerged stray branches   (no worktree, carry commits not in master)

Approve:
  (a) Remove MERGED + STALE worktrees + merged stray branches (default — never touches unique work)
  (b) Also remove selected UNIQUE worktrees (you pick which UNIQUE)
  (c) Show me each UNIQUE worktree's / unmerged branch's log before deciding
  (d) Abort — don't remove anything
```

Show the contents of `unique.txt` AND `branches_unmerged.txt` (with each branch's
ahead-count) so the user can see exactly what carries unmerged work before choosing.

Apply the user's choice. Removal uses the junction-safe teardown documented in **Abandoning a worktree** above — copy that block, plug in the worktree path as `WORKTREE_DIR` and the branch name as `SESSION_TAG`, and run it for each one. The hard rules carry over: pre-unlink the mingw junction with `cmd //c "rmdir ..."`, verify it's gone, only then `git worktree remove --force`. Never `rm -rf` a worktree path directly.

For removal-loop sanity:

```bash
# After user approval, iterate the chosen bucket(s). Example for MERGED + STALE.
for entry in $(cat "${REPORT_DIR}/merged.txt" "${REPORT_DIR}/stale.txt"); do
    WT="${entry%%|*}"
    BR="$(echo "${entry}" | cut -d'|' -f2)"

    # Pre-unlink junction via PowerShell reparse delete (cmd //c "rmdir" fails on
    # this host — see TEARDOWN SAFEGUARDS). Removes only the link, never the target.
    powershell.exe -NoProfile -Command "\$p='${WT}/td5mod/deps/mingw' -replace '/','\\'; if (Test-Path \$p){ \$i=Get-Item \$p -Force; if (\$i.Attributes -band [IO.FileAttributes]::ReparsePoint){ \$i.Delete() } }" 2>/dev/null

    # Verify junction is gone before --force; if it's still there, SKIP this
    # worktree and continue to the next, do NOT --force through a live junction.
    if [ -d "${WT}/td5mod/deps/mingw" ]; then
        echo "SKIP ${WT}: junction pre-unlink failed; inspect manually"
        continue
    fi

    git worktree remove --force "${WT}" 2>&1 || echo "SKIP ${WT}: worktree remove failed"

    # Branch deletion — -d for MERGED (refuses if branch is unmerged into master,
    # which is the safety net we want), -D ONLY for STALE because a STALE branch
    # by definition has no commits past the fork point so -d would already accept it.
    if [ "${BR}" != "<detached>" ]; then
        git branch -d "${BR}" 2>/dev/null || git branch -D "${BR}" 2>/dev/null || true
    fi
done

# Merged stray branches (no worktree) — always part of option (a) and above.
# -d refuses anything not fully in master, so it's a backstop even though we
# already filtered to merged ones. Unmerged stray branches are NEVER deleted
# here — they're surfaced in branches_unmerged.txt for the user to decide.
while read -r B; do
    [ -n "${B}" ] || continue
    git branch -d "${B}" 2>/dev/null && echo "deleted merged stray branch: ${B}"
done < "${REPORT_DIR}/branches_merged.txt"

# Final cleanup: prune any leftover .git/worktrees/ admin entries for paths
# that no longer exist on disk (happens when a worktree was rm'd ad-hoc by
# someone bypassing this recipe). Safe — only touches the git-internal registry.
git worktree prune --verbose
```

**Constraints on the janitor:**
- NEVER auto-run as part of a normal `/fix`. Only on explicit user request.
- ALWAYS show the categorized counts and wait for approval before any removal.
- The default action (a) NEVER touches UNIQUE worktrees — those might hold candidate fixes the user is iterating on across days/weeks.
- Honor LOCKED entries; never try to break a lock. If the user is sure a lock is from a crashed session, they should delete the `.git/worktrees/<name>/locked` marker manually and re-run.
- The recipe is idempotent: re-running on a clean tree reports zero in every bucket and removes nothing.

## Testing the build (INI + log loop)

There are two kinds of validation, and they're handled differently:

- **Self-verifiable checks** (the build loads, doesn't crash, a screenshot shows the right HUD/menu/layout/color, a CSV trace sweep matches the original, the log shows the expected branch/value): **you run these yourself** — do NOT ask the user to launch the game for them. The flow is: configure `td5re.ini` **inside the worktree**, run the auto-trace harness from the worktree, then read the worktree's log.

- **Checks that need the user's hands on the controls** (anything only a human driving/playing can judge — handling feel, "do the brakes bite", proportional analog throttle, reverse behavior, AI difficulty / catchup feel, multiplayer with real controllers / press-to-join, subjective audio or visual quality while actually driving): **ASK THE USER FIRST and wait until they say they're ready.** Do all the self-verification you can up front, then surface a short, explicit test brief and pause — see [When the fix needs the user to test](#when-the-fix-needs-the-user-to-test-ask-first) below. Never silently assume the user will get to it later, and never burn through to "shipped" on a fix whose only real proof requires them at the keyboard.

Because every `/fix` session runs in its own worktree, your INI tweaks and log output are guaranteed not to collide with other sessions.

### When the fix needs the user to test (ask first)

If confirming the fix genuinely requires the user to drive/play/use-their-controllers, do this **instead of** silently moving on:

1. Finish every check you *can* do yourself first (build sanity, screenshots, CSV sweep, logs) so the user's manual run is the *last* gate, not the first.
2. Prepare the manual-drive build so it's ready the moment they are: copy `td5re.exe` → `manual_drive.exe` in the worktree (a name not starting with `td5re` is invisible to any stray name-based kill — see line ~1008 and `feedback_fix_parallel_session_hazards_2026-06-02.md`), with the baseline INI flags written (`PlayerIsAI = 0`, `AutoRace`/`SkipIntro` as appropriate, audio per the SFX rule).
3. **Then ask** — a short message that states (a) that a manual test is needed, (b) the one-line test brief (what to do, what "correct" looks like), (c) the exact command/path to launch, and (d) "tell me when you're ready / once you've tried it." Use `AskUserQuestion` if a simple ready/not-ready or scenario choice helps; otherwise just ask in prose. **Wait for the user.** Do not proceed to commit-as-confirmed, merge, or "shipped" claims on the strength of an untested manual gate.
4. When the user reports back, read the relevant worktree log (see [Read logs when iterating](#read-logs-when-iterating)) before any further change.

The point is timing: the user wants to be **at the keyboard when the test is needed**, not surprised by a "please go test this" after the fact. Ask early enough that they can line up the run.

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
| `[Audio]` | `SFXVolume` | `0` | **Default for every `/fix` run** — silence sound effects so several test windows running side-by-side don't bleed overlapping engine/menu audio across sessions. **EXCEPTION:** when the fix focus is `audio` / the sound subsystem, set a real level instead (master default is `10`, or pass `--SFXVolume=N`) so you can actually hear the behavior under test. Music is untouched (`MusicVolume` keeps its INI value) |

**Track selection rule:** Parse `$ARGUMENTS` for a track name. If it mentions one of the quickrace mapping entries (Moscow, Edinburgh, Sydney, Blue Ridge, Jarash, Newcastle, Maui, Courmayeur, Honolulu, Tokyo, Keswick, San Francisco, Bern, Kyoto, Washington, Munich, Cheddar, Montego, Bez, Drag Strip), look up its index in `re/tools/quickrace/td5_quickrace.ini` and set `DefaultTrack` accordingly. Otherwise use `0` (Moscow). Moscow is the default because it's the reference track used by `/diff-race` and every regression baseline.

**PlayerIsAI exception:** Do NOT auto-flip to `1`. The only cases where you should ever set `PlayerIsAI = 1` in `/fix` are when the user explicitly asks to test AI behavior on slot 0 (attract-mode parity, AI rubber-banding, script VM). Never for general physics / camera / HUD / render fixes.

**SFX-volume rule:** Every `/fix` launch runs with `[Audio] SFXVolume = 0` by default (set it in the worktree INI alongside the other baseline keys). Multiple `/fix` windows running at once otherwise blast overlapping engine/menu audio, which is both confusing and a real annoyance for the user driving one of them. The ONLY exception is a fix whose focus is the **sound subsystem** (the `audio` bucket — e.g. the stuck-sound overload, frontend fade SFX, per-car engine audio): there, set `SFXVolume` to a real level (or pass `--SFXVolume=N` on the launch) so the audio under test is audible. `MusicVolume` is never forced — it keeps whatever the INI says.

The worktree INI is seeded from the committed master copy, which usually has these keys at the wrong defaults — so the full set of edits is needed on every fresh worktree. Do NOT launch the exe until all seven keys are written to disk.

Useful keys for fix-validation runs:

| Section | Key | Effect |
|---|---|---|
| `[Game]` | `SkipIntro = 1` | Bypass legal/intro FMVs |
| `[Game]` | `AutoRace = 1` | Boot straight into a race (no menu input) — **disable for /diff-race** |
| `[Game]` | `DefaultTrack` / `DefaultCar` / `DefaultGameType` | Pick the scenario for AutoRace |
| `[Game]` | `DebugOverlay = 1` | On-screen counters for physics/render |
| `[GameOptions]` | `Laps`, `Traffic`, `Cops`, `Difficulty`, `Collisions`, `Dynamics` | Race-rules toggles |
| `[GameOptions]` | `PlayerIsAI = 1` | Slot 0 driven by AI (mirrors original demo mode) — **disable for /diff-race** so player-path fixes are visible |
| `[Trace]` | `RaceTrace = 1` | Emit per-module `log/race_trace_*.csv` files |
| `[Trace]` | `RaceTraceSlot` | Which actor slot to record |
| `[Trace]` | `RaceTraceMaxFrames` | Cap trace length |
| `[Trace]` | `AutoThrottle = 1` | Hold throttle automatically once the race starts |
| `[Trace]` | `Modules` | CSV subset of `frame,pose,motion,track,controls,progress,view,calls` (or `*`) — narrow per fix focus |
| `[Trace]` | `Stages` | CSV subset of `frame_begin,pre_physics,post_physics,post_ai,post_track,post_camera,post_progress,frame_end,pause_menu,countdown` (or `*`) |
| `[Trace]` | `TraceFastForward` | Speed multiplier for trace runs (`1.0`=real-time, `4.0`=4x, `0.5`=half) |
| `[Logging]` | `Enabled = 1` | **Required** — turn on the centralized logger so `engine.log` / `race.log` / `frontend.log` populate (otherwise post-run reads see only `[session start]`) |
| `[Logging]` | `MinLevel` | `0`=DEBUG, `1`=INFO (default), `2`=WARN, `3`=ERROR. Bump to `2` to keep WRN/ERR but kill the INF spam |
| `[Logging]` | `Frontend` / `Race` / `Engine` / `Wrapper` | Per-sink gates. Turn off the categories you didn't touch to keep the log lean. `Wrapper` is the biggest emitter — leave at `0` unless debugging the D3D shim |

For ad-hoc scenario tweaks without rewriting the worktree INI, `td5re.exe` accepts `--Key=N` CLI overrides (case-insensitive, match INI names exactly). CLI > INI > defaults, applied in `main.c:td5_apply_cli_overrides()`. Useful when two parallel `/fix` sessions want different scenarios without racing on the INI file — just vary the `-ArgumentList` on each worktree's launch. Full key list in `reference_td5re_cli_overrides.md`; track/car number tables in `re/tools/quickrace/td5_quickrace.ini` (still the reference even though td5re.exe no longer reads that INI — the Frida launcher for the original binary still does). Example: `--DefaultTrack=15 --DefaultGameType=0 --AutoRace=1`.

### Pick trace modules + stages from the fix focus (MANDATORY)

The trace harness is modular: each module emits its own `log/race_trace_<module>.csv`. **Picking only the modules + stages relevant to the fix is mandatory** — capturing all eight modules at every stage at 30 Hz × 6 actors costs real FPS and floods the comparator with columns the fix doesn't touch.

Before launching, classify `$ARGUMENTS` into ONE of the focus buckets below and write the matching `Modules` + `Stages` lines into `${WORKTREE_DIR}/td5re.ini` `[Trace]`. If the fix straddles two buckets, union them. If you are genuinely unsure, default to `physics` — it is the largest sensible superset for race-frame work.

| Focus | Trigger keywords in `$ARGUMENTS` | `Modules` | `Stages` |
|---|---|---|---|
| `physics` | physics, suspension, integrator, friction, gravity, velocity, spring, damper | `pose,motion,track,frame` | `pre_physics,post_physics,post_track` |
| `collision` | collision, V2V, wall, contact, clipping, push, impulse | `pose,motion,track` | `post_physics,post_track` |
| `ai` | AI, route, branch, steering, rubber-band, script VM, target | `controls,motion,track` | `post_ai,pre_physics,post_physics` |
| `controls` | input, throttle, brake, gear, transmission, handbrake | `controls,motion` | `pre_physics,post_physics` |
| `camera` | camera, chase, orbit, view, FOV, trackside, spline | `view,pose,frame` | `post_camera,post_physics,frame_end` |
| `progress` | checkpoint, lap, finish, position, race-order, timer | `progress,track,frame` | `post_progress,post_track,frame_end` |
| `frontend` | frontend, menu, screen, button, HUD, overlay, dialog, language, gallery | `frame` | `frame_begin,frame_end` (often skip Frida capture entirely — see `feedback_fix_render_only_skip_frida.md`) |
| `render-only` | render, draw, UV, atlas, blit, texture, dispatch, tpage | `frame` | `frame_end` |
| `audio` | audio, sound, FX, music, mix, DSound | `frame,controls` | `frame_end,post_progress` |

Example INI block for a physics fix:
```ini
[Trace]
RaceTrace = 1
RaceTraceSlot = 0
RaceTraceMaxFrames = 600
AutoThrottle = 1
TraceFastForward = 4.0
RaceTraceMaxSimTicks = 450
Modules = pose,motion,track,frame
Stages = pre_physics,post_physics,post_track
```

For a render-only fix, set `Modules = frame` and `Stages = frame_end` — most render bugs are visible in the screenshot loop without per-tick CSV at all, and you can skip the original-side Frida capture (`feedback_fix_render_only_skip_frida.md`).

`--TraceModules=` and `--TraceStages=` CLI overrides accept the same CSV syntax and beat the worktree INI, so parallel sessions can capture different focus buckets without racing on the INI file.

### Launch the game (auto-close harness)

**Always drive the race via `AutoRace = 1` + `SkipIntro = 1`. Never use `SendKeys F5` — it's unreliable, and the AutoRace path is the supported harness.** Set those two flags in `${WORKTREE_DIR}/td5re.ini` before launching.

Launch from the worktree so it picks up the worktree's `td5re.exe`, INI, and log dir — NOT the main tree. **Setting `TD5RE_WINDOW_TITLE` before launching is MANDATORY on every launch path** (the auto-close harness below, the visual-verify variant, and each window of the multi-track sweep). This regressed recently — windows came up with the default caption — so treat it as non-negotiable: a launch without a composed title is a bug.

**The title must LEAD with a precise test instruction — not a raw echo of `$ARGUMENTS`.** The user reads it off the title bar to know what *this specific window* is for. Author a short imperative `TEST_FOCUS` (≤ ~90 chars) describing the concrete thing to look at and what "correct" looks like, derived from the fix + the Step-1 RE findings. Examples:

| `$ARGUMENTS` | Good `TEST_FOCUS` |
|---|---|
| "fix brake gate on reverse" | `reverse out of spawn — brakes should bite, no free-roll` |
| "gray out paint for paintless TD5 cars" | `SELECT CAR — paintless TD5 cars show greyed paint arrows` |
| "multiplayer traffic/police toggle not working" | `MP race, Traffic=off — zero traffic cars should spawn` |
| "fps/ms overlay position" | `check FPS/MS overlay sits top-left in BOTH menu and race` |

```bash
# TEST_FOCUS = a precise, human-authored "what to verify" string. Do NOT just
# truncate $ARGUMENTS — this is the one-line test brief the user reads off the
# title bar. Keep it imperative, concrete, and short (front-loaded so it survives
# Windows title truncation).
TEST_FOCUS="<one concise imperative: what to look at / what correct looks like>"

# Title bar leads with the test focus, then the session tag for disambiguation.
WIN_TITLE="TD5RE TEST: ${TEST_FOCUS}  [${SESSION_TAG}]"

cd "${WORKTREE_DIR}" && TD5RE_WINDOW_TITLE="${WIN_TITLE}" powershell.exe -Command "
\$env:TD5RE_WINDOW_TITLE = '${WIN_TITLE}'
\$proc = Start-Process -FilePath '.\td5re.exe' -PassThru
# Record OUR pid in a worktree-scoped file (never a shared /tmp name that a
# parallel session would clobber). Everything that kills or screenshots a
# td5re must target THIS pid — never the process name. See 'PID ownership' below.
Set-Content -Path '.td5re_test_pid' -Value \$proc.Id
Write-Host \"Launched td5re.exe PID=\$(\$proc.Id) (pid file: .td5re_test_pid)\"
Start-Sleep -Seconds 10
\$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
# Termination is PID-scoped to the \$proc WE launched. Defence-in-depth: only kill
# when its exe path is under THIS session's worktree, so even a recycled PID can
# never let us terminate a sibling /fix session's td5re. NEVER Stop-Process -Name
# / taskkill /IM — those hit every session's exe at once.
if (!\$proc.HasExited) {
    if (\$proc.Path -like '*${SESSION_TAG}*') { \$proc.Kill() }
    else { Write-Host \"REFUSING to kill PID=\$(\$proc.Id): path '\$(\$proc.Path)' is not under ${SESSION_TAG} — not ours\" }
}
Remove-Item '.td5re_test_pid' -ErrorAction SilentlyContinue
Write-Host 'Done - process closed'
" 2>&1
```

If `${TEST_FOCUS}` / `${WIN_TITLE}` contains single quotes, escape them for the PowerShell string (double them up: `'` → `''`). Keep the title under ~180 chars — Windows truncates beyond that anyway, so the test focus must come first.

Tune the inner `Start-Sleep` to your scenario — ~10s for grid/countdown/spawn/early-drive checks; **60+s** for slope/ramp/jump features further into the track (the first ~10s is always grid + countdown).

### PID ownership — never touch another session's td5re

> **HARD KILL RULE.** Terminate ONLY the PID this session launched — the `$proc` object from `Start-Process -PassThru`, or the PID stored in `${WORKTREE_DIR}/.td5re_test_pid`, and only after confirming its exe `.Path` is under this session's worktree (`*${SESSION_TAG}*`). **Never** `taskkill /IM td5re*`, `taskkill /F /IM`, `Stop-Process -Name td5re`, or "kill all td5re windows" — those reap every parallel `/fix` session's race mid-capture. There is no scenario in `/fix` where a name-based or unscoped kill is correct.

Multiple `/fix` sessions run `td5re.exe` simultaneously. Two operations corrupt sibling sessions when they target the process by **name** or by **"whatever window is up"** instead of the exact PID this session launched:

1. **Killing.** A name-based `Stop-Process -Name td5re` / `taskkill /IM td5re*` kills **every** session's exe — including races other sessions are mid-capture on. This has broken manual drive-tests twice (`feedback_fix_parallel_session_hazards_2026-06-02.md`). **Never blanket-kill by name.**
2. **Screenshots.** `capture_window.ps1 -Proc td5re` would grab the *first* td5re Windows enumerates — likely a sibling's window — and foreground/screenshot the wrong race.

Both are now enforced through the PID the launch harness wrote to `${WORKTREE_DIR}/.td5re_test_pid`:

- **Kill only your own PID.** The harness above already scopes `CloseMainWindow()`/`Kill()` to the `$proc` it started and deletes the pid file on exit. To reap an orphan left by a *previous turn of THIS session*, read the pid and kill only that — with a defence-in-depth check that the process's exe path is under your worktree (a sibling's td5re has a different `.Path`):
  ```bash
  cd "${WORKTREE_DIR}"
  PID=$(cat .td5re_test_pid 2>/dev/null)
  [ -n "$PID" ] && powershell.exe -Command "
    \$p = Get-Process -Id $PID -ErrorAction SilentlyContinue
    if (\$p -and \$p.Path -like '*${SESSION_TAG}*') { \$p.Kill(); Write-Host 'killed own pid $PID' }
    else { Write-Host 'pid $PID is not ours (path mismatch) — leaving it alone' }"
  rm -f .td5re_test_pid
  ```

- **Screenshot only your own window.** Pass the PID — never `-Proc`/`-TitleLike` — so capture foregrounds and grabs THIS session's race. `capture_window.ps1` now **refuses** an ambiguous name/title match (more than one td5re window) and tells you to pass `-ProcessId`, so a name-based capture can no longer silently grab a sibling's window. `capture_window.ps1` lives ONLY in the main tree (`tools/` is gitignored → it is NOT checked out into worktrees), so call it by its absolute main-tree path — it's host-level (screenshots by PID), so running main's copy from a worktree is correct:
  ```bash
  cd "${WORKTREE_DIR}"
  pwsh "C:/Users/maria/Desktop/Proyectos/TD5RE/tools/capture_window.ps1" -ProcessId "$(cat .td5re_test_pid)" -Out "shot_${SESSION_TAG}.png"
  ```

- **Visual-verify variant (launch → screenshot → kill).** The auto-close harness above closes the window after its `Start-Sleep`, so to grab a frame you launch **detached** (no auto-close), record the pid, capture by pid, then kill that pid:
  ```bash
  cd "${WORKTREE_DIR}"
  # Re-declare the test-focus title (separate shell block — same string you authored
  # for the launch step) so this window's caption also says what to verify.
  TEST_FOCUS="<same concise what-to-verify string as the launch step>"
  WIN_TITLE="TD5RE TEST: ${TEST_FOCUS}  [${SESSION_TAG}]"
  TD5RE_WINDOW_TITLE="${WIN_TITLE}" powershell.exe -Command "
    \$proc = Start-Process -FilePath '.\td5re.exe' -PassThru
    Set-Content -Path '.td5re_test_pid' -Value \$proc.Id
    Write-Host \"PID=\$(\$proc.Id)\""
  sleep 12   # let it reach the frame you want
  pwsh "C:/Users/maria/Desktop/Proyectos/TD5RE/tools/capture_window.ps1" -ProcessId "$(cat .td5re_test_pid)" -Out "shot_${SESSION_TAG}.png"
  # PID-scoped stop + worktree-path guard so a recycled PID can never hit a sibling.
  powershell.exe -Command "\$p = Get-Process -Id $(cat .td5re_test_pid) -ErrorAction SilentlyContinue; if (\$p -and \$p.Path -like '*${SESSION_TAG}*') { \$p.Kill() }"
  rm -f .td5re_test_pid
  ```

(For a *user-driven* manual play session that must survive the whole time, copy `td5re.exe`→`manual_drive.exe` and launch that — a name not starting with `td5re` is invisible to any stray name-based kill, per `feedback_fix_parallel_session_hazards_2026-06-02.md`. **Reminder:** if the fix can only be confirmed by the user driving/playing, don't just hand them the exe after the fact — **ask first and wait until they're ready**, per [When the fix needs the user to test](#when-the-fix-needs-the-user-to-test-ask-first).)

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

## Frida preflight (HARD STOP — required before any original-binary launch)

**Why:** `re/tools/quickrace/td5_quickrace.ini` is shared mutable state. A previous `/re` or interactive session may have left it pointed at a frontend screen, a non-zero spawn offset, `frontend_only=true`, or `player_is_ai=true` — in which case the Frida-side Original CSV captured by the sweep below is **garbage** (no race ever runs, or runs on the wrong scenario / wrong driver). A single empty/stale Original CSV silently invalidates the whole fix's evidence basis. `/diff-race` is protected via `--no-ini`; `/fix` is not, so we gate it here.

**Run this check before EVERY sweep, even when the sweep already passes `--set` overrides** — the goal is to catch surprise drift, not to second-guess the overrides.

```bash
QR_INI="C:/Users/maria/Desktop/Proyectos/TD5RE/re/tools/quickrace/td5_quickrace.ini"

# Read the four fields that silently break the sweep when non-default.
SCREEN=$(awk -F= '/^[[:space:]]*frontend_screen[[:space:]]*=/    {gsub(/[[:space:]]/,"",$2); print $2; exit}' "${QR_INI}")
ONLY=$(awk -F= '/^[[:space:]]*frontend_only[[:space:]]*=/        {gsub(/[[:space:]]/,"",$2); print $2; exit}' "${QR_INI}")
OFFSET=$(awk -F= '/^[[:space:]]*start_span_offset[[:space:]]*=/  {gsub(/[[:space:]]/,"",$2); print $2; exit}' "${QR_INI}")
PISAI=$(awk -F= '/^[[:space:]]*player_is_ai[[:space:]]*=/        {gsub(/[[:space:]]/,"",$2); print $2; exit}' "${QR_INI}")

DRIFT=()
[ "${SCREEN:-X}" != "-1" ]    && DRIFT+=("frontend_screen=${SCREEN} (must be -1 — anything else jumps the original to that frontend screen, NO race runs)")
[ "${ONLY:-X}"   != "false" ] && DRIFT+=("frontend_only=${ONLY} (must be false — true keeps the original in the menu, NO race runs)")
[ "${OFFSET:-0}" != "0" ]     && DRIFT+=("start_span_offset=${OFFSET} (must be 0 — non-zero shifts the spawn down-track and breaks tick-0 parity with the port)")
[ "${PISAI:-X}"  != "false" ] && DRIFT+=("launcher.player_is_ai=${PISAI} (must be false unless this fix is specifically about AI slot 0; otherwise physics traces are skewed)")

if [ "${#DRIFT[@]}" -gt 0 ]; then
    echo "FRIDA PREFLIGHT FAILED — ${QR_INI} has non-default knobs:"
    printf '  - %s\n' "${DRIFT[@]}"
    echo ""
    echo "These will silently corrupt the Original CSV in the sweep below."
    echo ""
fi
```

**Decision rules** when `DRIFT` is non-empty:

1. **Default action — STOP and ask the user, verbatim:**
   ```
   FRIDA PREFLIGHT FAILED — re/tools/quickrace/td5_quickrace.ini has non-default knobs:
     <bullet list of the DRIFT items printed above>

   These knobs are usually leftovers from a /re or interactive session.
   For this /fix, do you want me to:
     (a) Reset the INI to safe defaults
         (frontend_screen=-1, frontend_only=false, start_span_offset=0, player_is_ai=false)
         and proceed with the sweep [DEFAULT for any physics/AI/track/camera/HUD fix]
     (b) Keep these knobs because this /fix is specifically testing them
         (e.g. frontend rendering, mid-track spawn, AI-on-slot-0 parity).
         Confirm which knobs are intentional.
     (c) Abort this /fix.
   ```

2. **Never silently proceed.** The `--set` overrides in the sweep loop *do* mask the INI for that specific run, but proceeding without surfacing the drift hides the fact that the user's environment is in an unexpected state — and any side-channel Frida probe (e.g. `--extra-script`, manual relaunch) the user adds later will still inherit the bad INI.

3. **If the user picks (a) "reset",** rewrite the four keys in place using `Edit`:
   ```
   Edit ${QR_INI}: frontend_screen = -1
   Edit ${QR_INI}: frontend_only = false
   Edit ${QR_INI}: start_span_offset = 0
   Edit ${QR_INI}: player_is_ai = false
   ```
   Then re-run the preflight block and confirm `DRIFT` is empty before proceeding.

4. **If the user picks (b) "keep",** they MUST name which knobs are intentional. Print the resulting plan back to them ("OK, keeping `frontend_only=true` because this fix targets the main menu; resetting the other three.") and edit the non-confirmed ones to safe defaults before the sweep.

5. **If the user picks (c) "abort",** stop the workflow cleanly. Leave the worktree alone — the user can resume later or tear it down with the **Abandoning a worktree** recipe.

**Skip exception:** Frontend / asset / build-tooling fixes that have already declared they are skipping the multi-track sweep entirely (per Step 2 rule 5) also skip this preflight — there is no Frida launch to protect.

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

# Reuse the SAME concise TEST_FOCUS you authored for the single-track launch so
# every sweep window's title bar still says what to verify. Re-declare it here —
# this is a separate shell block, so the variable from the launch step is gone.
TEST_FOCUS="<same concise what-to-verify string as the launch step>"

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
    TD5RE_WINDOW_TITLE="TD5RE TEST: ${TEST_FOCUS} — ${TRACK_NAME} [${SESSION_TAG}]" powershell.exe -Command "
    \$proc = Start-Process -FilePath '.\td5re.exe' -PassThru
    Set-Content -Path '.td5re_test_pid' -Value \$proc.Id   # OUR pid; kill scoped to it, NEVER by name
    Start-Sleep -Seconds 20   # 20s covers countdown + ~14s of race
    \$proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 2
    # PID-scoped kill + worktree-path guard: only ever terminate THIS session's exe.
    if (!\$proc.HasExited) {
        if (\$proc.Path -like '*${SESSION_TAG}*') { \$proc.Kill() }
        else { Write-Host \"REFUSING cross-session kill of PID=\$(\$proc.Id)\" }
    }
    Remove-Item '.td5re_test_pid' -ErrorAction SilentlyContinue"
    cp log/race_trace.csv "${BUNDLE_DIR}/fix_track${TRACK_N}_${TRACK_NAME}.csv"

    # --- Original side: run TD5_d3d.exe under Frida from main ---
    cd C:/Users/maria/Desktop/Proyectos/TD5RE
    rm -f log/race_trace_original.csv
    # Pin the shared INI fields that /re and ad-hoc frontend probes commonly leave
    # in a state that silently breaks /fix Frida runs:
    #   - frontend.frontend_only=true  → hook stays in the menu, no race launches
    #   - race.start_span_offset=N>0   → spawn moves N spans down the track
    #   - launcher.player_is_ai=true   → slot 0 driven by AI (skews physics traces)
    # Always override these explicitly so the sweep is deterministic regardless of
    # what's currently on disk in re/tools/quickrace/td5_quickrace.ini.
    python re/tools/quickrace/td5_quickrace.py --trace --trace-auto-exit \
        --set race.track=${TRACK_N} --set race.car=0 --set race.game_type=0 --set race.laps=1 \
        --set race.start_span_offset=0 \
        --set frontend.frontend_only=false \
        --set frontend.frontend_screen=-1 \
        --set launcher.player_is_ai=false \
        --trace-max-frames 300 --max-runtime 90
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

**Auto-close is mandatory.** Every invocation of `td5_quickrace.py` from `/fix` (or any orchestrated context) MUST have at least one of:
- `--trace --trace-auto-exit` — the trace script signals close once `RaceTraceMaxFrames` / `RaceTraceMaxSimTicks` fires.
- `--max-runtime <SECONDS>` — wall-clock cap. Default in the launcher is `180`, but the sweep pins `--max-runtime 90` per track so a stuck Frida session can't hold a worktree's td5re.exe / TD5_d3d.exe past the bundle window.

Never pass `--max-runtime 0` from `/fix` — that disables the cap and lets a hung TD5_d3d.exe survive the session, which blocks worktree teardown and silently corrupts the next sweep iteration. `--max-runtime 0` is reserved for interactive Ghidra/Frida debugging, where the user is supervising the process by hand.

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
- On approval, Step 4 is the **same single-shot robust merge as `/end`**: merge under the `mkdir` lock, build-once-deploy-by-copy (the worktree's recheck-built exes are the deliverable — never a redundant post-merge rebuild), push with reject-retry, then **immediate junction-safe teardown**. Resolve any rebase/merge conflict **in this session** (preserve both intents, build to verify); escalate only on genuinely incompatible-intent conflicts.
- Run every self-verifiable check yourself (build, screenshots, CSV sweep, logs). But when confirming the fix genuinely requires the user's hands on the controls (driving/playing feel, real controllers, subjective audio/visual), **ask first and wait until they're ready** — prep `manual_drive.exe`, give a one-line test brief, and don't claim the fix confirmed/shipped on an untested manual gate. See [When the fix needs the user to test](#when-the-fix-needs-the-user-to-test-ask-first).
- If the research agent can't find the relevant function, ask the user for hints (function name, address, or module) before retrying.
- If build fails after edits, try fixing compile errors directly (up to 2 attempts), then escalate to user.
- If the user returns to a session later and `${WORKTREE_DIR}` / `${SESSION_TAG}` are no longer in scope, re-derive them with `git worktree list` and pick the branch matching this session's fix.
