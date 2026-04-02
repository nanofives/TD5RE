# TD5RE Research Workflow

Research original binary behavior via Ghidra MCP, delegated to a subagent to keep main context clean.

**Usage:** `/re <function name, address, or behavior to investigate>`

## Workflow

Launch a **general-purpose Agent** to investigate the original binary. The agent prompt MUST include:

- The research query: `$ARGUMENTS`
- Instructions to connect to the analyzed Ghidra project:
  ```
  Use mcp__ghidra__project_program_open_existing with:
    project_location="C:/Users/maria/Desktop/Proyectos/TD5RE"
    project_name="TD5"
    program_name="TD5_d3d.exe"
    read_only=true
  ```
- Instructions to find the function(s) using `mcp__ghidra__function_by_name`, `mcp__ghidra__search_text`, or `mcp__ghidra__function_at` (if an address is given)
- Instructions to decompile with `mcp__ghidra__decomp_function`
- Instructions to trace call graph with `mcp__ghidra__function_callees` / `mcp__ghidra__function_callers`
- Optionally check structs with `mcp__ghidra__layout_struct_get` or globals with `mcp__ghidra__decomp_global_rename`

**Return format**: Compact summary (under 500 words):
1. Function name(s) and address(es)
2. Decompiled logic: key constants, formulas, state machine, coordinate math
3. Data structures used (field offsets, strides)
4. Call graph context (who calls it, what it calls)

Present the summary to the user. No edits, no implementation.
