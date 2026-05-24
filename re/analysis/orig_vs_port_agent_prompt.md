# Orig-vs-port verdict agent procedure

You audit the TD5 reverse-engineering project for orig-vs-port faithfulness. The
original Win32 binary `TD5_d3d.exe` is being source-ported to `td5re.exe` (under
`td5mod/src/td5re/`). For each function you are assigned, classify the port
implementation against the orig and emit one CSV row.

## Verdict taxonomy

| Verdict | Meaning |
|---|---|
| `FAITHFUL` | Port matches orig byte-for-byte OR with only semantic-preserving refactors (rename, struct reshape, comment-only diff). All constants, control-flow branches, side-effects, callees preserved. |
| `INTENTIONAL` | Port diverges, but the divergence matches a class tag in `re/analysis/orig_vs_port_intentional_divergences.md` (e.g. `D3D11_BACKEND`, `MSVC_RAND_OVERRIDE`, `DXPTYPE`). Cite the tag. |
| `OVERSIGHT` | Port diverges in a way that looks like a bug — missing write, wrong constant, swapped branch, off-by-one, etc. If the divergence is already documented in `re/analysis/permanent_l4_residual.md` as `REGR#1`–`REGR#6`, cite the regression ID. |
| `NOT_PORTED` | No port equivalent found. If orig is a CRT/M2DX/FMV stub, also note the intentional class tag (`MINGW_CRT`, `M2DX_AUDIO`, `FMV_STUB`, etc.). |
| `CANNOT_DETERMINE` | Divergence visible but verdict requires runtime trace, Frida probe, byte-level instrumentation, or context the agent does not have. |

## Workflow (per function)

1. **Load Ghidra MCP tool schemas** (once at start, before any per-function work):
   ```
   ToolSearch with query="select:mcp__ghidra__project_program_open_existing,mcp__ghidra__decomp_function,mcp__ghidra__function_report,mcp__ghidra__function_callers,mcp__ghidra__function_callees"
   ```

2. **Acquire a Ghidra pool slot** (once at start; reuse for all assigned functions):
   ```
   bash scripts/ghidra_pool.sh acquire    # returns slot name like TD5_pool3
   ```

3. **Open the program (READ-ONLY — HARD RULE per CLAUDE.md):**
   ```
   mcp__ghidra__project_program_open_existing(
     project_location="C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_pool",
     project_name="<slot from step 2>",
     program_name="TD5_d3d.exe",
     read_only=true
   )
   ```
   Capture the returned session_id for subsequent calls.

4. **Per function — decompile orig:**
   ```
   mcp__ghidra__decomp_function(session_id=<id>, function_start=<address>)
   ```
   Capture: key constants, control-flow shape, side-effects, callees, struct field offsets touched.

5. **Find port match.** Try in order:
   - `Grep` for the address in port code: pattern `<bare hex>` (e.g. `004168B0`) AND `0x<padded>` (e.g. `0x004168B0`), glob `td5mod/src/td5re/**/*.{c,h}`. Port convention is to cite the orig address in a comment near the port impl.
   - If not found by address, `Grep` for the orig function name.
   - If still not found, grep for unique string constants / callee names from the orig body.
   - If genuinely nothing matches → likely `NOT_PORTED`.

6. **Read port code** at any hits (use `Read` with line offset/limit). Compare against orig.

7. **Classify** per the taxonomy. When in doubt between INTENTIONAL and OVERSIGHT, check `re/analysis/orig_vs_port_intentional_divergences.md` for class tags and `re/analysis/permanent_l4_residual.md` for known regressions.

8. **Release pool slot at end** (always — even on errors):
   ```
   bash scripts/ghidra_pool.sh cleanup
   ```

## Anti-hallucination rules

- **Port citations must be `file:line`.** If you cannot find the port code, you cannot cite it — say `NOT_PORTED` instead.
- **Orig citations must include the Ghidra address.** Function names alone are unreliable (port renames freely).
- **Don't trust names — read bodies.** A port function with the same name may differ; one with a different name may be the actual port.
- **<60% confidence → `CANNOT_DETERMINE`.** Don't guess.
- **Prior memory is a hint, not proof.** `MEMORY.md`, prior L5 reports, and the divergences doc are leads — verify against current code (`git log` it's already stale, etc.).
- **Tool result discipline.** If a `Grep` returns 0 hits, that's evidence — don't claim a port match exists.

## Output format

Return ONLY raw CSV rows in your final text response (no markdown fences, no preamble, no postamble). One row per assigned function, in the assigned order:

```
<address>,<name>,<body_size>,<prior_level>,<verdict>,<tag>,<port_file:line>,<evidence>,AGENT,2026-05-24
```

Field rules:
- `address` — preserve as given (e.g. `00403a20`, no `0x` prefix to match confidence map).
- `name` — preserve as given.
- `verdict` — one of: `FAITHFUL`, `INTENTIONAL`, `OVERSIGHT`, `NOT_PORTED`, `CANNOT_DETERMINE`.
- `tag` — class tag for INTENTIONAL / NOT_PORTED, regression ID for OVERSIGHT (`REGR#N`) or short label, empty for FAITHFUL/CANNOT_DETERMINE.
- `port_file:line` — single best port anchor (e.g. `td5_physics.c:5631`). Empty for NOT_PORTED.
- `evidence` — ≤220 chars. Quote any commas with double-quotes around the whole field. Mention key orig vs port detail (constant value, missing branch, etc.).
- `AGENT` and `2026-05-24` literal.

Example rows (illustrative):
```
00403c80,ComputeReverseGearTorque,258,5,FAITHFUL,,td5_physics.c:4127,"orig 0x403c80 reverse-gate constant 0x100 + steering_cmd write at +0x30C matches port; no divergence",AGENT,2026-05-24
004168b0,RaceTypeCategoryMenuStateMachine,520,4,OVERSIGHT,REGR#1,td5_frontend.c:6383,"button 3 → game_type=7 in port vs orig 9; button 4 → 9 in port vs orig 7 (swap)",AGENT,2026-05-24
00448157,_rand,33,0,NOT_PORTED,MINGW_CRT,,"MSVC CRT stub in orig; port uses MinGW CRT with td5_msvc_rand.c LCG override",AGENT,2026-05-24
```

## On failure / blocked

If any step fails (Ghidra error, pool exhausted, decompile timeout), still emit a row with `verdict=CANNOT_DETERMINE` and `evidence` describing the blocker. NEVER return nothing — the harness needs one row per assigned function.

Release any acquired pool slot before returning, even on failure.
