# Trace Hook Specs — point Ghidra functions into Frida

`/diff-race hooks=<file>` takes a YAML (or JSON) spec file that describes
extra functions to trace on BOTH sides of a diff. The Frida script attaches
via `Interceptor.attach` on the original; the port emits matching rows via
`TD5_TRACE_CALL_ENTER` / `TD5_TRACE_CALL_RET` macros you insert in the C
source. Both sides write to `log/calls_trace*.csv` with the same schema, so
the comparator can match rows key-by-key.

## Spec file format

```yaml
hooks:
  - name: suspension_step           # REQUIRED; must match the port-side macro
    original_rva: 0x00403A20        # REQUIRED; module base is TD5_d3d.exe = 0x00400000
    args: 3                         # number of int32 args to capture (0..8)
    capture_return: true            # if true, also emit retval column
    # port_symbol is informational — you insert TD5_TRACE_CALL_* manually
    port_symbol: td5_physics_suspension_step
    # notes: free-form comments (ignored)
    notes: |
      Per-wheel spring-damper step. Args: actor_idx, wheel_idx, is_rear.
      Return: new wheel_y_fp24_8.
```

## Row schema (both sides produce the same columns)

```
sim_tick, fn_name, call_idx, n_args,
arg_0, arg_1, arg_2, arg_3, arg_4, arg_5, arg_6, arg_7,
has_ret, ret
```

`call_idx` is a per-(sim_tick, fn_name) counter so the Nth call to the same
function in the same sim tick lines up on both sides. If the original and
port disagree on the **number** of calls per tick, you'll see "Missing from
..." lines in the comparator output — that itself is a bug signal.

## Port-side: add a macro

In the C source, at the function you want to trace:

```c
int td5_physics_suspension_step(int actor_idx, int wheel_idx, int is_rear) {
    TD5_TRACE_CALL_ENTER("suspension_step", actor_idx, wheel_idx, is_rear);
    /* ... body ... */
    int result = /* computed */;
    TD5_TRACE_CALL_RET("suspension_step", result, actor_idx, wheel_idx, is_rear);
    return result;
}
```

- The `name` string must match the spec file.
- Pass 1..8 args (cast to `int32_t` if needed; the macro casts the first one
  for you). If the function is nullary, pass a dummy `0`.
- Zero runtime cost when tracing is disabled.
- Remove the macros when you're done investigating (or guard with
  `#ifdef TD5_TRACE_CALLS` if you want to keep them around).

## Frida-side: handled automatically

The orchestrator (`diff_race.py`) passes the spec to the Frida launcher,
which injects `HOOK_SPECS` into `tools/frida_race_trace.js`. On each hook,
Frida reads up to 8 `args[k].toInt32()` values and (if `capture_return`)
the return value, emits a row to `log/calls_trace_original.csv`.

**Args are read as raw int32 from the x86 stack** (first stack arg at
`[esp+4]` onward, standard `__cdecl`/`__stdcall` layout). For pointers,
floats, or structs passed by value, the CSV column will contain the raw
32-bit bit pattern — reinterpret on the port side if you need matching
semantics.

## Usage

```bash
# One-off
python tools/diff_race.py --hooks re/trace-hooks/suspension.yaml

# With all the usual flags
python tools/diff_race.py --car 0 --track 0 --seconds 10 \
       --hooks re/trace-hooks/collision_normal.yaml
```

Via the slash command:

```
/diff-race hooks=re/trace-hooks/suspension.yaml
```

## Output

- `log/calls_trace_original.csv` — rows emitted by Frida on `TD5_d3d.exe`.
- `log/calls_trace.csv` — rows emitted by `td5re.exe` via the macro.
- The comparator runs the standard race-trace diff PLUS a second diff on
  these two files, keyed by `(sim_tick, fn_name, call_idx)`.

## Debugging tips

- If the Frida side emits rows but the port doesn't, the macro is either
  not present in the port's copy of the function OR the function never ran.
  Check `log/race_trace.csv` for sim_ticks around where you expected the
  call.
- If call counts differ per tick, the two implementations have diverged in
  a way that changes how often this function gets invoked — often a
  loop-bound or early-out condition mismatch.
- Per-call rows add up fast (100s per tick for physics hooks). Scope your
  captures by picking a short `--seconds` window and filtering to specific
  tick ranges via `--key-fields sim_tick,fn_name,call_idx` plus a post-hoc
  `awk`/`head` on the CSVs.
