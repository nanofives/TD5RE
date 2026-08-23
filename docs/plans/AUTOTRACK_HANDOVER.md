# AUTO-GENERATED track — handover: what is pending

Phase 1 shipped on master at `62ee46fb` (pushed, verified on the remote).
Self-test smoke 12/12 PASS; merged tree builds clean dev+release with the
structure lint at baseline.

Everything below is what is NOT done, split by whether it needs testing,
verifying, or building. Companion docs:
`AUTOTRACK_STREAMING.md` (Phase 2 design), `AUTOTRACK_BRANCHES.md` (branch spec).

---

## 1. NEEDS A HUMAN TO LOOK (nothing is blocked on code)

Eleven screenshots were produced for review and **never reviewed**. They are in
`.happy-share/cmt0vdzbk23aen51clghwr0rf/` (`01_city_start.png` …
`11_sky_forest.png`, plus `README.txt`). All from one pinned seed (123456789),
so differences between them are real.

Open questions I could not answer myself:
- Do CITY / INDUSTRIAL / FIELDS / FOREST read as genuinely different places?
- Are buildings still too tall or too close? Setback and heights were each
  adjusted once, judged from a single frame.
- Do the tree billboards hold up, or read as flat cards?
- **A pale diagonal streak on the near road surface** appeared in one biome
  frame and was never investigated. Possibly UV or z-fighting on the closest
  road quads.

## 2. CONFIRMED RENDERING — both features work (both still default OFF)

Confirmed 2026-08-22 via the live-control socket (unique port 37071, pinned seed
123456789, capture armed with `TD5RE_D3D12_CAPTURE=1`, all logs cleared first) —
NOT the wall-clock `TD5RE_FRAMEDUMP` timer, which had failed twice because the
car cleared the transient feature before the timer fired. Frames are in
`.happy-share/cmt559lxd3loqn51cpmgenpb5/`.

**Tunnels** — `TD5RE_AUTOTRACK_TUNNELS=1`. CONFIRMED: renders as a proper
enclosed tunnel you drive through — side walls both sides + a roof running
down-track, `COL: NONE` inside. Evidence: `tunnel_confirmed.png` (span 83).
- **Required-knob fact that was MISSING from this doc and cost discovery time:**
  tunnels are MODELS.DAT box meshes, so they need **`TD5RE_AUTOTRACK_SCENERY=1`
  in addition to `_TUNNELS=1`** — scenery is opt-in, and `_TUNNELS=1` alone
  emits nothing visible.
- The gate `tg_span_in_tunnel` returns 0 for `si <= GRID_SPAN+40` (=64), so the
  **first visible tunnel run is spans 80-99**, not the earlier 40-59 run.
- Correcting the earlier overstated risk: the "might read as a wall with no
  MOUTH" prediction was WRONG — it reads as a real tunnel. The
  no-interior-darkening prediction WAS right (there is none, but it still reads
  fine without it).

**Branch forks** — `TD5RE_AUTOTRACK_BRANCHES=1`. CONFIRMED. Logs match the spec
(`branch fork=600 corridor=1801..1840 base=601`, `jumps=1`, `spans=1841`).
Characterised honestly: a **gentle parallel bow-and-rejoin** — `tg_branch_shift`
ramps the offset 1 -> 2.6 road-widths over 40 spans — **not a sharp Y**, and it
photographs poorly because road and ground are the same grey. The real proofs
are the F12 collision wireframe from on the branch (`branch_fork_confirmed.png`,
two distinct railed corridors), the on-corridor daylight shot
(`branch_corridor_daylight.png`, normalized span 620 — a road that only exists
because of the fork), and the `lanes=8` junction widening
(`branch_fork_junction_wide.png`, main 4 + branch 4).

**Promotion (recommendation, NOT applied in this commit):** both stay default
OFF here. Tunnels are safe to promote whenever scenery is on (they depend on
`_SCENERY=1`). The branch is currently a one-way soft merge, not a true
recombining split — better held until it is upgraded to a real type-11 rejoin
(see the dual-carriageway plan in the acute/dual-lane work), then shipped
together.

## 3. PARTIAL AGAINST THE ORIGINAL REQUEST

**"acute angle curves"** — the honest ceiling is ~176° (an 88° per-section
heading budget), not the ~160° first written here. A `±80°` clamp keeps the
spine non-trapping; a dedicated ACUTE budget widened to 88° still has
`cos(88°) > 0`, so forward progress — and the non-trapping / no-self-intersection
proof — survive. A TRUE ≥180° down-track hairpin is mathematically INCOMPATIBLE
with any single-axis monotone-progress guarantee (forward progress is zero at
90°) and is deliberately not offered. **The catch:** `ADJACENT_SKIP` must be
DERIVED from the budget (~116 spans at 88°), not the hardcoded 25 — the 25
silently voids the guarantee once the budget rises above ~80°. (Implemented,
uncommitted, in a worktree as of 2026-08-23 — pending review + frame verify.)

**"dual lanes"** — variable road WIDTH, not variable lane COUNT. Consecutive
spans share a vertex row and a shared row has one point count, so lanes are
fixed at 4. A real carriageway split needs the same dedicated-row trick the
fork uses — PLUS a new type-11 REJOIN span so the two carriageways recombine
(the branch fork currently only one-way merges).

## 4. PHASE 2 STREAMING — far less blocked than first written

The one substantial part of the original request with no implementation: generate
on the fly, buffer to view distance, unload behind. Design in
`AUTOTRACK_STREAMING.md` — a fixed-size ring buffer presented as a CIRCUIT,
which dissolves three of the four blockers (int16 ceiling, route realloc, AI
wrap) and leaves the minimap.

Landed: **S1** (addressable range emitter; composability proven byte-identical)
and **S2** (deterministic regeneration + an in-place rewrite gate).

**S3 was reported BLOCKED; investigation narrowed it to a NON-blocker.** The S2
gate fails only because it compares the WHOLE ring, including two loader-patched
sentinels:

    stream selfcheck: FAIL -- live span array differs from regen at span 0

The single mutating pass is `td5_track_bind_runtime_pointers()` (`td5_track.c`,
called right after `load_strip`). It patches exactly TWO spans: span[0] →
`span_type 1→9` (SENTINEL_START) + `link_prev -1→ring-1`; span[ring-1] →
`span_type 1→10` (SENTINEL_END) + `link_next -1→0`. It derives everything from
`ring` (`g_td5.track_span_ring_length`) — NO runtime-pointer dependency; the blob
/ vertex / attribute pointers are wired separately in `load_strip`. **Every
INTERIOR span is byte-identical to regen**, so the memcpy assumption is CORRECT
for the interior regions a stream cursor actually rewrites.

Ruled out with reasons: the type-11 link fixup (`td5_assetsrc.c:523`) runs only
in the JSON pack-on-load path and touches only `span_type==11` records, which the
generator never emits; the drag geometry passes are gated on `drag_race_enabled`,
which autotrack never enters.

**Resolution: option (b) — re-apply bind's sentinel patch to the rewritten
region (a region-scoped sentinel helper already exists in `td5_track.c`, taking
first/count/last), OR scope the S2 comparison to interior spans and assert the
two sentinels separately.** Do NOT bake sentinels into `tg_emit_span_range`
(option a): that corrupts every interior mid-ring rewrite, which MUST stay
`span_type=1` — and interior rewrites are the streaming hot path. Enable the gate
with `-DTD5RE_AUTOTRACK_STREAM_SELFCHECK` (compile-time) plus
`TD5RE_AUTOTRACK_SELFCHECK=1`.

Remaining stages: S3 live geometry (ring-buffer write cursor + seam invariant —
a separate piece of work), S4 route bytes, S5 scenery via runtime display lists
(`build_span_strip_display_list`, `td5_track.c:3007`, is the seam), S6 minimap.

## 5. SMALLER LOOSE ENDS

- Terrain is **cosmetic** — collision comes from the STRIP, so leaving the road
  still drops you into nothing. Verges do not catch you.
- Ground skirts extend a fixed 24000 units; no horizon fill beyond that.
- One sky, borrowed from a shipped panorama chosen by seed. No per-biome sky.
- No `--SelfTest=1 --SelfTestSuite=1` (full matrix) run, only smoke.
- `Track index 60 out of range, no checkpoint data` is logged every race —
  harmless (checkpoint tables are keyed to shipped tracks) but noisy.

## 6. TRAPS FOR WHOEVER PICKS THIS UP

Read these before debugging anything here. Each cost real time.

1. **`airborne_mask`, not contact.** The race-trace column was named
   `wheel_mask` and reads `actor->damage_lockout`, which is actor+0x37C — an
   AIRBORNE mask where a set bit means the wheel is OFF the ground. Reading it
   as contact inverted every conclusion and caused two changes that had to be
   reverted. Renamed to `airborne_mask` with the polarity documented on the
   struct field; the misleading `damage_lockout` name is still in the actor
   struct.
2. **`ang_roll` is a RATE**, `actor->angular_velocity_roll`, not an angle.
3. **Clear ALL logs, not just `race.log`.** A stale `engine.log` produced a
   confident wrong conclusion during the final verification pass.
4. **Pin the seed** (`TD5RE_AUTOTRACK_SEED`) for any A/B. Without it each run is
   a different road and every comparison is meaningless — this invalidated a
   whole round of measurements.
5. **`re/tools/td5_trackgen.py` is buggy for branches** — it sets
   `branch_start = ring`, but `td5_track_branch_to_main_span` rejects
   `span <= ring`. Native tracks use `ring+1` with a pad span. Every other
   scenery step succeeded by porting that tool; this is the one place it must
   not be copied.
6. **Verify against a shipped track**, not intuition. Comparing angular rates
   with Moscow is what showed there was no defect at all, after two rounds of
   chasing a "tilt bug" that did not exist.

### Method notes (confirmed working 2026-08-22 — use these, don't rediscover)

7. **Position the car with `--StartSpanOffset=N`** — the reliable way to spawn
   fresh, lane-centred, at a chosen span. `get_state` reports the NORMALIZED
   span, so to land ON the branch corridor you need a RAW span >= 1801 (pick N
   accordingly, not the normalized number). Spawn PAST a grind point rather than
   driving through it — fresh spawns still grind the outer wall in tight curves.
8. **`hold_action throttle` needs `frames:0`** (indefinite hold). Short bursts
   auto-release too fast, so the car looks "stuck" while it is actually
   throttle-starved.
9. **Capture on demand, never on the wall-clock timer.** Drive with `--Control=1`
   + `scripts/td5re_mcp/` (or its stdlib `game_client.py`), position, then dump —
   the `TD5RE_FRAMEDUMP` wall-clock timer fires after the car has already cleared
   a transient feature (this is exactly why the fork went uncaught twice). The
   control port 37060 is GLOBAL across sessions — set a unique
   `TD5RE_CONTROL_PORT`. Capture must be armed with `TD5RE_D3D12_CAPTURE=1`.
10. **Broad `git status` / `find` / `grep` rooted at a `/fix`-style worktree is
    very slow.** `worktree_setup.ps1` COPIES ~11.9k `re/assets` files into each
    worktree, so a whole-tree scan stats all of them and can look hung for a
    minute (compounded by a pending approval prompt on the same command). Scope
    to tracked files (`git status --untracked-files=no`) or to a path like
    `td5mod/src`. This cost two apparent hangs.

## 7. QUEUED — GUARDRAILS (missing entirely; spotted in the frames)

Not ported: `re/tools/td5_scenery.py` has `make_guardrail_segment` (~:187) and a
`guardrails` entry in `normalize_spec` (page / height 750 / offset 120 /
left+right); none of it reached `td5_trackgen.c`.

Worth doing even though not functionally required: the collision walls ALREADY
exist and already contain the car (STRIP rail vertices, confirmed working), so
today the car is stopped by a boundary the player CANNOT SEE. Guardrails add no
collision — they make the existing constraint legible (a real gameplay
improvement, not decoration) and also fix the abrupt asphalt-to-grass verge
transition visible in `11_sky_forest.png`.

Should be cheap, with evidence: `tunnel_confirmed.png` already shows thin tall
boxes running along each verge oriented to the road tangent, rendering
correctly — geometrically a guardrail already. So: `tg_emit_box_mesh` per span
at ~width/2 lateral, ~750 tall, ~120 outward — the same call the tunnel walls
use, minus the roof. Mesh budget: +2/span on top of ground+road+building+bridge
should stay within `TG_MAX_MESHES_PER_ENTRY` (`SPANS_PER_ENTRY*8`) — CHECK it.

Judgement calls (decide, don't guess):
- TEXTURE: there is no metal/barrier page. Reusing `TD5_TG_PAGE_WALL` reads as a
  low concrete wall, not a crash barrier — accept for a first pass, or add a 5th
  page (emitters are simple, see `tg_emit_texture_page_wall`).
- PLACEMENT: exactly on the rail line so it coincides with where collision
  actually stops you (the whole point). Confirm against
  `td5_track_resolve_wall_contacts`' rail derivation (`td5_track.c:1288`, rails
  from left/right_vertex_index + the lane-count nibble) rather than assuming
  width/2 equals the rail.
- Probably NOT continuous everywhere — real barriers sit on the outside of bends
  and drops, not down every straight. Gate on curvature, or on the
  bridge/elevated case first (also where they matter most).
- SKIP inside tunnels (the walls already serve the purpose) and across the branch
  fork mouth (they would block the corridor entrance visually).

Priority: AFTER A's S3 gate fix. Default OFF behind a knob until frame-verified
via the control socket — drive into the barrier and confirm the visual boundary
coincides with where the car actually stops.
