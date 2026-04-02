# TD5RE Fix Workflow

Fix a bug or implement a feature in the TD5RE source port. Delegates Ghidra RE research to a subagent (keeps large MCP responses out of main context), then hands implementation to Codex.

**Usage:** `/fix <description of the bug or feature>`

## Workflow

### Step 1: Ghidra Research Agent

Launch a **general-purpose Agent** (has MCP access) to investigate the original binary. The agent prompt MUST include:

- The bug/feature description: `$ARGUMENTS`
- Instructions to connect to the analyzed Ghidra project:
  ```
  Use mcp__ghidra__project_program_open_existing with:
    project_location="C:/Users/maria/Desktop/Proyectos/TD5RE"
    project_name="TD5"
    program_name="TD5_d3d.exe"
    read_only=true
  ```
- Instructions to find and decompile the relevant function(s) using `mcp__ghidra__function_by_name` or `mcp__ghidra__search_text`, then `mcp__ghidra__decomp_function`
- Instructions to trace callers/callees with `mcp__ghidra__function_callees` / `mcp__ghidra__function_callers` if needed
- Instructions to read the current source port file(s) under `C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re/` for comparison
- **Return format**: The agent must return a COMPACT summary (under 500 words) containing:
  1. Which original function(s) were decompiled (name + address)
  2. The extracted logic: key constants, formulas, state transitions, coordinate math
  3. What the source port currently does differently (with file paths and line numbers)
  4. Specific changes needed to fix the bug

Do NOT ask the agent to make edits. Research only.

### Step 2: Codex Implementation Agent

Using the research summary from Step 1, launch a **codex:codex-rescue Agent** with:

- The full research summary (logic, constants, formulas)
- Exact file paths and line numbers to change
- The specific code changes needed
- Instruction to run the build after editing:
  ```
  cd C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re && ./build_standalone.bat
  ```
- Instruction to fix any compile errors

### Step 3: Verify

After Codex completes:
1. Read the changed files to confirm the edits look correct
2. Run the build yourself to double-check:
   ```bash
   cd C:/Users/maria/Desktop/Proyectos/TD5RE/td5mod/src/td5re && ./build_standalone.bat
   ```
3. Report the result to the user: what was found, what was changed, build status

### Step 4: Commit & Push

After the build passes, stage and push **all modified source files** in a single commit:

```bash
cd C:/Users/maria/Desktop/Proyectos/TD5RE

# Stage only source files (never stage build outputs, logs, or game data)
git add td5mod/src/td5re/*.c td5mod/src/td5re/*.h
git add td5mod/src/*.c td5mod/src/*.h 2>/dev/null || true

git commit -m "fix: <one-line description from $ARGUMENTS>

RE basis: <function name(s) and Ghidra address(es) from Step 1>

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"

git push origin HEAD
```

Rules for this step:
- **Always push** — do not skip even if the change is small.
- Use `git add` with explicit file paths, never `git add .` or `git add -A`.
- The commit subject must start with `fix:` and match the original `$ARGUMENTS` description.
- If `git push` fails (no remote, auth error, etc.), report the error to the user and stop — do not retry destructively.

## Key Rules

- The Ghidra research agent does ALL MCP work. Main context never sees raw decompilation output.
- Codex does ALL code edits (except trivial 1-2 line fixes).
- If the research agent can't find the relevant function, ask the user for hints (function name, address, or module) before retrying.
- If build fails after Codex edits, try fixing compile errors directly (up to 2 attempts), then escalate to user.
