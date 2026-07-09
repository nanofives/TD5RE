# TD5RE Multithreading Architecture — Design Plan (2026-06-08)

Goal: refactor TD5RE from a single-threaded engine toward a multithreaded
architecture, across **every system that genuinely benefits** — not only
split-screen pane submission. This document is grounded in a full read of the
current source (main loop, sim tick, D3D11 wrapper, physics/AI, netplay, asset
loader). It states the hard constraints first, then a phased plan ordered by
risk/reward.

> Note on RE basis: the original `TD5_d3d.exe` is a single-threaded 1999
> DirectDraw game — there is **no original-binary multithreading to reverse
> engineer**. The "research" here is the *current source-port architecture*.
> The Ghidra step of the /fix workflow is therefore N/A for this task.

---

## 0. The constraint envelope (what makes threading dangerous here)

Three hard constraints bound every design decision below.

### C1 — Netplay is strict input-lockstep with NO correction → the sim must be bit-identical across peers
- `td5_net.c` syncs **only** the 6 `control_bits[]` input masks + the
  host-authoritative `frame_dt` (`td5_net.c:2755,2785`), then every peer
  simulates the identical tick locally. There is **no per-frame state
  checksum or correction** — a single divergence desyncs the game permanently
  (only a coarse `resync_barrier` exists for topology changes).
- Implication: **any** nondeterministic reordering — parallel float
  accumulation (FP is non-associative), a thread race on shared actor state,
  or a sort whose tie-order depends on thread scheduling — breaks lockstep.
- Mitigating fact: core vehicle physics is **fixed-point 24.8** (integer,
  associative) — see `td5_physics_update_vehicle_actor` (`td5_physics.c:1065`).
  Integer per-actor math is reorder-safe. The danger is the FP and RNG below.

### C2 — Global `rand()` is order-dependent and is consumed inside the sim
- Shared CRT `_holdrand`, seeded once per race (`td5_game.c:1512`). The code
  **explicitly depends on consumption order** (`td5_game.c:1476-1499`).
- Live per-tick consumers: AI (`td5_ai.c:554`), VFX (`td5_vfx.c:1034-3035`),
  results (`td5_game.c:5809`). Plus implicit "RNG": traffic recovery reads
  slot-0 `world_pos_z` as a pseudo-random seed (`td5_ai.c:6472`).
- Implication: **`rand()` cannot be called from a parallel sim phase** without
  first replacing it with per-slot deterministic RNG streams. This is the
  single biggest blocker to threading the sim.

### C3 — D3D11 immediate context is single-thread-only; the wrapper's state model is serial-by-design
- Device is created **without** `D3D11_CREATE_DEVICE_SINGLETHREADED`
  (`d3d11_backend.c:881,959`) → it keeps its internal thread-safety lock.
  **This is already correct** for deferred-context recording; do NOT add the
  flag.
- No deferred contexts / command lists exist today — all draws go through the
  one immediate context.
- Three blocking hazards for parallel recording:
  1. **Shared ring offsets** `s_vb_ring_offset` / `s_ib_ring_offset`
     (`d3d11_backend.c:643-644`) mutated per draw, unsynchronized.
  2. **Single "current state" cache** `g_backend.state` + `current_*_idx`
     (`wrapper.h:127-168,262`) — a serial dirty-diff binder.
  3. **In-place vertex scaling of the caller's buffer**
     (`device3.c:581-590,642-651`) — not reentrant.

---

## 1. Sim-vs-render boundary (the clean seam)

The fixed-step sim loop is fully sequential and ends at `td5_game.c:4479`.
Everything from `:4481` (render interpolation + `td5_camera_finalize_all`)
through the per-pane render loop (`:4574`) is **render/frame work that reads a
now-frozen sim state**. The pane loop is the natural parallel unit.

```
td5re_frame()                              main.c:1312
  td5_game_tick()                          td5_game.c:857   (4-state FSM)
    td5_game_run_race_frame()              td5_game.c:3549
      update_frame_timing (accumulator)    :6273  (TD5_TICK_ACCUMULATOR_ONE=0x10000)
      ── SIM (fixed 30Hz, serial) ──────────────────────────  while accum>0xFFFF && ticks<4   :3696
         input → ai_tick → physics_tick → track_tick → vfx_tick
         → update_race_order → camera_solve_tick_all          :4321..4401
         consume one tick; simulation_tick_counter++          :4472,4478
      ── RENDER (per-frame, parallel candidate) ────────────
         g_subTickFraction recompute                          :4484
         td5_physics_apply_render_interpolation               :4498
         td5_camera_finalize_all                              :4509
         td5_render_begin_scene + ONE backbuffer clear        :4548,4556
         for vp in viewport_count:                            :4574  ← PARALLEL UNIT
            camera_apply_view(vp) → configure_projection
            begin_world_pass → render_actors_for_view(vp)
            vfx → flush_translucent/additive → hud_status_text
         td5_plat_present(1)   (ONCE per frame, not per pane) :4790
```

Per-pane render **reads** only frozen state (`g_actorSlotForView[vp]`, actor
table, `s_viewports[vp]`, `g_camWorldPos[vp]`, `g_subTickFraction`). Per-pane
render **writes** the shared single-context render module (deferred-additive
buffers, `s_current_texture_page`, the immediate context) — these must become
per-pane to parallelize. Camera-apply writes `g_camWorldPos[v]` /
`g_depthFovFactor` which are **already per-view-indexed** (safe to keep).

---

## 2. Foundational infrastructure (prerequisite for Phases A & B)

### F1 — Portable job system / thread pool  (`td5_jobs.c/.h`, NEW)
A small fixed-size Win32 worker pool (N = cpu_cores−2, clamped) with a
lock-free or mutex-guarded work queue, `td5_jobs_submit(fn, ctx)` and
`td5_jobs_wait(handle)` / a parallel-for `td5_jobs_parallel_for(n, fn)`.
Reused by Phase A (decode jobs) and Phase B (pane-record jobs). This is the
single foundational piece both high-value phases depend on.

### F2 — Deterministic per-slot RNG  (prerequisite for Phase C; valuable on its own)
Replace the implicit reliance on global `rand()` inside the sim with one
deterministic RNG stream **per actor slot** (e.g. xorshift/PCG seeded from
`race_seed ^ slot_index`). This both (a) unblocks any future sim threading and
(b) hardens cross-peer determinism **today** — the global-`rand()`-order
dependency is a latent lockstep fragility even single-threaded if any peer's
consumption order ever drifts. F2 is independently worth doing.

---

## 3. Phased plan

### Phase A — Background / parallel asset loading  ★ DO FIRST (low risk, high value)
**Why first:** entirely outside the sim and netplay → **zero determinism
risk**; builds the F1 job system; immediately cuts track-load and
frontend-screen latency.

- All loading is currently **synchronous/blocking** on the main thread
  (`td5_game.c` InitRace 33-step bootstrap; frontend PNGs on screen entry).
  No streaming, no pump loop — just one blocking loading-screen blit
  (`td5_game.c:6478`).
- **Decode-on-worker / upload-on-main** split (the only correct architecture):
  - Pure-CPU, embarrassingly parallel per asset: ZIP inflate
    (`td5_inflate.c` — already stateless), pack-on-load JSON→binary
    (`td5_assetsrc.c` `s_registry` encoders — independent per asset), texture
    decode→BGRA (`td5_asset_decode_png_rgba32` etc.), palette/colorkey/alpha
    passes. All use thread-safe `malloc` (no custom arena in the load path).
  - Context-affinity, must stay on main thread: GPU upload
    (`td5_plat_render_upload_texture` → `WrapperSurface_FlushDirty`, uses
    `g_backend.context` Map/Copy/UpdateSubresource) and writes to the shared
    1024-page texture table (`s_tex_*[1024]`, `td5_platform_win32.c:317`).
  - Workers produce ready BGRA buffers (+w/h/page); main thread drains a
    single-consumer queue and performs only the context-side upload.
- Deliverables: `td5_jobs` (F1), a load-job queue, a real loading-screen pump,
  parallel pack-on-load. Touch: `td5_asset.c`, `td5_assetsrc.c`, InitRace
  bootstrap in `td5_game.c`.
- Risk: LOW. Hazards are confined and known (the page table + the context
  upload), both already single-thread by construction.

### Phase B — Multithreaded render pane submission  ★ DO SECOND (medium risk, high value)
**Why:** the true scaling lever for N-way split-screen (the original 180fps
ask). Single/2-pane paths stay byte-identical.

- Model: each pane records into its **own `CreateDeferredContext`**; main
  thread `ExecuteCommandList`s them **in pane order** → one composite → one
  `Present`. One shared backbuffer RTV + viewport/scissor rects (already how
  panes composite today, `td5_platform_win32.c:4556`).
- Wrapper refactor required (resolve the C3 hazards):
  1. Per-context **render-state cache** — split `g_backend.state` +
     `current_*_idx` into a per-deferred-context struct.
  2. Per-thread (or index-partitioned) **dynamic VB/IB ring** — give each pane
     recorder its own ring offsets, or partition the 16MB VB / 4MB IB into N
     bands.
  3. Fix **in-place vertex scaling** — scale into the streamed copy, never
     mutate the caller's buffer.
  4. Per-pane copies of render-module globals (`s_deferred_add_*`,
     `s_current_texture_page`); camera per-view globals are already arrays.
- Fallback: keep the serial immediate-context path; gate the deferred path
  behind a flag (`[Render] ThreadedPanes`) and auto-fall-back when
  `viewport_count <= 2` or the driver reports no real command-list concurrency
  (`FinishCommandList` always works but may be driver-emulated → still correct,
  possibly not faster).
- Touch: `d3d11_backend.c`, `device3.c`, `wrapper.h`, `td5_platform_win32.c`,
  the pane loop in `td5_game.c`.
- Risk: MEDIUM. Large wrapper rewrite; concurrency benefit is driver-dependent;
  must preserve exact draw order within a pane for correct alpha.

### Phase C — Parallel per-actor physics integration  ⚠ DEFER (high risk, low value)
**Why defer:** S30 profiling already proved **physics is not a bottleneck** —
16 cars cost ~3.4ms by direct QPC; the "17ms post_physics" was a per-sub-tick
profiler artifact. So the upside is splitting ~3.4ms, against severe
determinism risk.

- Only `td5_physics_update_vehicle_actor` (`td5_physics.c:1065`) is
  **parallel-safe** (own-state-only, fixed-point, no `rand()`).
- **SERIAL-ONLY** (do not thread): V2V collision + anti-tunnel
  (`td5_physics_resolve_vehicle_contacts` `:4729`, Gauss-Seidel 24-round
  read-modify-write pairs) and `update_race_order` sort (`td5_game.c:5984`).
- Hard preconditions before ANY sim threading: **F2 per-slot RNG**, plus
  double-buffering of cross-actor reads (traffic reads all actors' span/lane;
  recovery reads slot-0 world_z), plus running **single-threaded whenever
  netplay is active** (or proving the partition is bit-identical).
- Recommendation: do F2 regardless (determinism hardening); implement C only
  if a real per-actor cost appears (e.g. far more than 16 actors).

### Phase D — Already-threaded / not worth it
- **Netplay**: already on a worker thread (`td5_net.c:1985`). Done.
- **Sound**: DirectSound does its own mixing thread; per-car param updates are
  cheap. Skip.
- **FMV**: stub with an Interlocked skip flag. Skip.

---

## 4. Recommended order & gating

1. **F1 job system** + **Phase A async loading** — safe, immediately useful,
   foundation for B.
2. **Phase B threaded pane submission** — reuses F1; delivers the split-screen
   scaling; flag-gated with serial fallback.
3. **F2 per-slot RNG** — determinism hardening, independently valuable.
4. **Phase C** — only if measured need; requires F2 + double-buffering + a
   net-active serial fallback.

Each phase is independently shippable and flag-gated so the serial path
remains the always-correct reference.

## 4b. IMPLEMENTATION STATUS (2026-06-08, branch fix-1780921498-…)

Committed + verified on the worktree branch:
- **F1 job pool** (`td5_jobs.c/.h`) — fork/join Win32 pool, 14 workers, serial
  fallback, `TD5RE_JOBS=N` kill-switch. Commit `fed5bbf`.
- **Phase A** parallel texture-page decode — London 178→57ms (**3.1×**), Moscow
  29→23ms; 293/293 & 475/475 byte-identical. Commit `fed5bbf`.
- **Phase B Stage 1** — `td5_render.c` re-entrancy: per-pane scratch bundled in
  `RenderScratch`, reached via `__thread g_rs` (defaults to one static
  instance). `#define s_foo (g_rs->foo)` shims redirect all access sites.
  Serial path byte-identical (user drive-test confirmed: single + split render
  normally). Commit `a6f78b4`.
- **Phase B Stage 2a** — wrapper per-pane deferred-context plumbing:
  `WrapperRecCtx` (deferred ctx + private VB/IB + private viewport/fog CBs +
  private state cache + SRV) + `__thread g_wrapper_rec` (NULL ⇒ immediate
  path). Routed: `Backend_StreamUpload/ApplyStateCache/SelectPixelShader/
  UpdateFogCB/UpdateViewportCB` and `td5_plat_render_draw_tris/draw_lines/
  set_preset/set_fog/bind_texture/set_viewport/set_clip_rect`. Pool API
  `Backend_RecPoolEnsure/RecBegin/RecEnd/RecExecute/RecPoolRelease`. Serial
  path verified byte-identical (London RENDERSTAT unchanged). Commit `eee07b4`.

**Remaining — Stage 2b (flip threading on). Blocked by a newly-scoped gate:**
1. **VFX + HUD re-entrancy.** The translucent/projected/deferred-additive
   buckets (now in `g_rs`) are filled by BOTH `render_actors_for_view` AND the
   VFX draws, then flushed once — so the whole pane must record on one thread;
   VFX + HUD cannot stay serial-on-main. `td5_vfx.c` and `td5_hud.c` therefore
   need their non-per-view mutable scratch made re-entrant (HUD `s_cur_view/
   s_cur_flags/s_cur_scale/s_metric_value/s_is_first_player`; VFX
   `s_current_view_index`, tire-track/smoke/taillight pools + cursors,
   `s_emitter_desc_count`). The `[TD5_MAX_VIEWPORTS]`-indexed statics are
   already pane-safe. Approach: same `__thread`-bundle/shim as Stage 1, OR
   index the offenders by pane.
2. **Per-pane `g_rs` pool** in `td5_render.c`: `td5_render_scratch_pool_ensure(n)`
   + `td5_render_scratch_bind(i)/unbind()` (lazy-calloc N `RenderScratch`).
   Workers bind their instance; verify the translucent free-list / depth-bucket
   init runs per-pane (cross-pane init audit).
3. **Threaded pane loop** in `td5_game.c`: `td5_jobs_parallel_for` over panes →
   each worker `Backend_RecBegin(i,rect)` + bind `g_rs[i]` → full pane body →
   `Backend_RecEnd`. Main thread re-binds the swap RTV, then `Backend_RecExecute(i)`
   in pane order, then present. Serial fallback for `viewport_count<=2` or
   `!ThreadedPanes` or `RecPoolEnsure`/`jobs` unavailable.
4. **`configure_projection` caching audit** — it early-outs when dims are
   unchanged; each worker instance must be force-initialized so a fresh pane
   instance isn't left with no projection.
5. **`[Render] ThreadedPanes` INI knob** (default 0) + release clamp.

Stage 2b correctness is ONLY validatable by a threaded drive-test (headless
proves no-crash only). Default-off keeps single/2-pane on the byte-identical
serial path verified above.

## 5. Verification strategy per phase
- **A**: byte-identical loaded assets vs serial path (the DAT-retirement
  `--selftest-assetsrc` already gives a byte-exact oracle); load-time wall-clock
  before/after; headless AutoRace smoke.
- **B**: per-pane render is deterministic in output (same command stream,
  reordered only across panes which composite to disjoint scissor rects) →
  screenshot parity at 1/2 panes (must be identical to serial), FPS at 6/9
  panes; the existing `spectate_sweep.ps1` harness + RENDERSTAT.
- **C**: `/diff-race` bit-identical CSV trace vs serial single-thread on the
  trio (Moscow/Newcastle/random), with **netplay-active** lockstep soak as the
  gate. Any divergence = stop.
