# Precise-Port Batch Workflow

**Created:** 2026-05-13
**Goal:** Run N parallel port-from-disassembly sessions against `TD5_d3d.exe`, each owning one simulation-core function from `precise_port_scope.md`. Capture Frida traces in batches, diff per-function, and merge only when per-tick byte-equality holds.

## Constraints (from existing infra)

- **Ghidra pool**: `scripts/ghidra_pool.sh` already provides 4 read-only slots (`TD5_pool0..3`). Each slot is a clone of `TD5.rep`; `acquire`/`release` is lock-file based.
- **Frida**: cannot run multiple `TD5_d3d.exe` instances reliably (DDraw exclusive, DSound conflicts, drag-strip already known to crash). **One game process at a time.** But Frida can attach **multiple scripts** to one process — that's how we batch.
- **Trace harness**: `td5_trace.h` is modular (bitmask per family, CSV per file). The pattern generalizes: add a per-function emit row (e.g. `TD5_TraceFunc_<addr>Row`) and a hot-path gate.
- **Diff**: `tools/diff_physics_trace.py` pattern (load → key by `(tick, stage)` → per-column divergence count + first-diverge) generalizes; we just key by `(tick, slot, callsite)` instead.

## Process distinguishability — naming convention

Every artifact gets a `pool<N>_<addr>` tag so parallel sessions never collide:

| Artifact | Path |
|----------|------|
| Ghidra slot | `ghidra_pool/TD5_pool<N>.rep` (existing) |
| Frida probe | `tools/frida_pool<N>_<addr>.js` |
| Original trace | `log/orig/pool<N>_<addr>.csv` |
| Port trace | `log/port/pool<N>_<addr>.csv` |
| Diff report | `log/diff/pool<N>_<addr>.txt` |
| Port worktree | `worktree/precise-<addr>/` (git worktree per function) |
| Worker log | `log/worker/pool<N>_<addr>.log` |

`<addr>` is the 8-hex original address (e.g. `00403720`). `<N>` is the pool slot. The two together uniquely identify the work-in-flight and let `ghidra_pool.sh status` show which function each slot is handling.

## Per-function lifecycle

```
[Phase 1: ACQUIRE]                    one pool slot
  ghidra_pool.sh acquire  →  TD5_pool<N>
  git worktree add worktree/precise-<addr> -b precise-<addr>
  echo "<addr>" > ghidra_pool/TD5_pool<N>.assigned   # claim marker

[Phase 2: STATIC PORT]                read-only Ghidra + write to worktree
  - Open program in pool<N> (read_only=true)
  - Read disassembly listing (NOT just decompiled C — listing is ground truth)
  - Note FPU control word, x87 vs SSE intermediates, sign/width promotions
  - Port from listing into worktree/precise-<addr>/td5mod/src/td5re/td5_<module>.c
  - Build the worktree (dry-run; no merge yet)

[Phase 3: GENERATE FRIDA PROBES]      append to batch queue
  - Identify input set: args + actor fields read
  - Identify output set: actor fields written + return value
  - Emit tools/frida_pool<N>_<addr>.js writing to log/orig/pool<N>_<addr>.csv
  - Add a matching emit point to port: td5_trace_emit_func_<addr>()
    writing log/port/pool<N>_<addr>.csv
  - Append the probe path to log/_batch_queue.txt

[Phase 4: BATCH CAPTURE]              serial — one race per batch
  - Drain log/_batch_queue.txt: attach all queued Frida scripts to TD5_d3d.exe
    via the existing /diff-race harness
  - Play one deterministic race (same track/seed/inputs used by port-side)
  - Detach; each probe has written its slot of the trace

[Phase 5: DIFF + GATE]                parallel — one diff per pool
  python tools/diff_func_trace.py log/port/pool<N>_<addr>.csv log/orig/pool<N>_<addr>.csv
  Acceptance: zero divergent rows across captured window.
  - PASS  → merge worktree to master, release pool slot
  - FAIL  → return to Phase 2 with diff as guide; iterate

[Phase 6: RELEASE]
  rm ghidra_pool/TD5_pool<N>.assigned
  ghidra_pool.sh release <N>
  git worktree remove worktree/precise-<addr>
```

## Parallelism model

- **Phase 1–3 (port + probe gen) — parallel.** Up to 4 pool slots × independent worktrees. Each Claude session/subagent owns one slot. No game required.
- **Phase 4 (capture) — serial.** Single TD5_d3d.exe. A batch drains the queue: 6–12 functions per race is realistic given Frida overhead (~1 ms/probe/tick).
- **Phase 5 (diff) — parallel.** Pure file I/O, no shared state.

Throughput target: **6 functions/day** sustained (1 batch capture + 6 ports running in parallel through phases 2–3 + iterations on failures). At 60 functions total, **~2 weeks calendar** if uninterrupted. Real number will depend on cost-per-function — pilot will measure this.

## Why batching by Frida (not by Ghidra)

The expensive resource is **the running game** (we can only run one at a time, and races take 30–120 seconds each). The cheap resource is **Ghidra pool slots** and **port worktrees** (just disk + read-only state). So we parallelize the slow human/agent work (port from listing) and serialize only what must be serial (the game capture).

## Generalizing the trace harness

`td5_trace.h` already has 9 modules. To add per-function tracing without proliferating modules:

1. Add one new module `TD5_TRACE_MOD_FUNCSITE = 0x200` with a generic row:
   ```c
   typedef struct TD5_TraceFuncRow {
       uint32_t addr;       /* 0x00403720 etc — identifies the function */
       int      slot;
       int      n_words;
       int32_t  words[16];  /* generic key/value capture — schema per-function */
   } TD5_TraceFuncRow;
   ```
2. Each port function instrumented with `td5_trace_emit_func(addr, slot, ...)` at entry+exit.
3. CSV: `log/port/funcsite.csv` with `frame,sim_tick,addr,slot,phase,w0,w1,...,w15`.
4. Diff splits by `addr` column → one logical per-function CSV without N file handles.

Alternative: one file per function (matches the per-pool naming convention above). Tradeoff: more file descriptors, simpler diff. Pilot will decide.

## Open questions for the pilot

1. **FPU control word**: does original set 53-bit vs 64-bit precision? `_controlfp` audit needed — divergence here invalidates every float comparison.
2. **Disassembly listing access**: do we read from Ghidra MCP (`listing_disassemble_function`) or pre-export per-function `.lst` files? Listing access cost dominates Phase 2.
3. **Determinism**: `/diff-race` already pins seed/inputs. Confirm the original-side capture is identical-tick-aligned with port-side (it has been for `physics_trace`).
4. **Frida script ceiling**: how many `Interceptor.attach` can we layer before tick-rate drops below the 30 Hz sim cap? Unknown — needs measurement.

## Pilot recommendation

**Function 0x00403720 — RefreshVehicleWheelContactFrames** (per-wheel probe).

Why:
- Concrete open residual: Edinburgh span 433–434 chassis launch (memory `todo_edinburgh_span_433_residual_launch`).
- Existing Frida adjacent: `frida_wheel_contact_probe.js`, `frida_uphill_wheel_probe.js`, `frida_per_wheel_state_probe.js` → fast probe authoring.
- HIGH leverage: gates `0x004057F0 UpdateVehicleSuspensionResponse` downstream — if probe is wrong, every suspension fix is on sand.
- Bounded scope: 4 wheels × ~30 fields → ~120 columns of state — large but well-defined.
- Already partially shipped: `todo_wheel_world_positions_hires_second_transform` fixed one bug here; pilot will surface what's left.

Pilot deliverable: time-to-zero-diff (including harness build) measured in hours. If > 16 hours, narrow scope before scaling. If 4–8 hours, we proceed to batch B (collision) next.
