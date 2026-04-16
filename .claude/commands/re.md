# TD5RE Research Workflow

Research original binary behavior via Ghidra MCP, delegated to a subagent to keep main context clean.

**Usage:** `/re <function name, address, or behavior to investigate>`

## Workflow

Launch a **general-purpose Agent** to investigate the original binary. The agent prompt MUST include:

- The research query: `$ARGUMENTS`
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
- Instructions to find the function(s) using `mcp__ghidra__function_by_name`, `mcp__ghidra__search_text`, or `mcp__ghidra__function_at` (if an address is given)
- Instructions to decompile with `mcp__ghidra__decomp_function`
- Instructions to trace call graph with `mcp__ghidra__function_callees` / `mcp__ghidra__function_callers`
- Optionally check structs with `mcp__ghidra__layout_struct_get` or globals with `mcp__ghidra__decomp_global_rename`
- **NO-GUESSING RULE** (CRITICAL — include verbatim in every research agent prompt):
  ```
  You are doing deterministic reverse engineering. Follow these rules strictly:
  - Report ONLY what the decompilation literally shows. Never infer, speculate, or "fill in gaps."
  - For every constant, offset, or formula you report: include the exact Ghidra address where it appears.
  - If a value's meaning is unclear, report it as a raw hex/decimal constant — do NOT assign semantic meaning.
  - If a function's purpose is unclear, describe it mechanically (reads offset X, calls Y, writes Z) — do NOT guess its intent or name it.
  - Never use words like "probably", "likely", "seems to", "appears to", "I think", "presumably."
  - If something is uncertain, mark it [UNCERTAIN] and state exactly what evidence is missing.
  - When reporting sign-sensitive values (coordinates, offsets, velocities): always report BOTH the raw hex AND the signed decimal interpretation.
  ```
- **If TGA assets are involved**: verify actual file dimensions by reading the TGA header — never derive dimensions from `create_surface` calls in Ghidra (those are compositing buffers, not asset sizes)
- **If frontend surface drawing is involved**: report the `Copy16BitSurfaceRect` flag for each surface — `0x10` = opaque blit, `0x11` = color-key transparent blit

**Return format**: Compact summary (under 500 words):
1. Function name(s) and address(es)
2. Decompiled logic: key constants, formulas, state machine, coordinate math
3. Data structures used (field offsets, strides)
4. Call graph context (who calls it, what it calls)
5. **Blit modes** (if applicable): which surfaces are opaque vs color-key
6. **Asset dimensions** (if applicable): actual TGA header W×H, not compositing buffer sizes

Present the summary to the user. No edits, no implementation.
