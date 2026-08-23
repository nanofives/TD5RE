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

## 2. IMPLEMENTED BUT NEVER CONFIRMED IN A FRAME (both default OFF)

**Tunnels** — `TD5RE_AUTOTRACK_TUNNELS=1`. Emitted, but no frame has ever shown
one rendering, working or broken. Once misreported as broken; that was a tall
building being misidentified. Known risks from the format survey, not from
observation: no engine support for interior darkening/occlusion, and every span
in a run gets an identical cross-section so there is no tunnel MOUTH.

**Branch forks** — `TD5RE_AUTOTRACK_BRANCHES=1`. Structurally validated: jump
arithmetic correct (`corridor=1801..1840 base=601 ring=1800`, so B0 maps to main
601), no out-of-bounds, zero wall rejections after the seam fix. But the fork
itself has never appeared in a frame — the framedump fires on a wall-clock timer
and the car cleared span 600 before it triggered, twice. Use the control socket
(`--Control=1`, `scripts/td5re_mcp/`) to dump on demand at a chosen span instead
of guessing a start offset.

## 3. PARTIAL AGAINST THE ORIGINAL REQUEST

**"acute angle curves"** — you get switchbacks up to ~160°, not hairpins that
double back down-track. The ±80° heading clamp is what makes self-intersection
geometrically impossible; removing it reintroduces walk trapping (rejection
sampling delivered 300 spans of 1800). Any change here must keep a non-trapping
guarantee.

**"dual lanes"** — variable road WIDTH, not variable lane COUNT. Consecutive
spans share a vertex row and a shared row has one point count, so lanes are
fixed at 4. A real carriageway split needs the same dedicated-row trick the
fork uses.

## 4. NOT BUILT: PHASE 2 STREAMING

The one substantial part of the original request with no implementation: generate
on the fly, buffer to view distance, unload behind. Design in
`AUTOTRACK_STREAMING.md` — a fixed-size ring buffer presented as a CIRCUIT,
which dissolves three of the four blockers (int16 ceiling, route realloc, AI
wrap) and leaves the minimap.

Landed: **S1** (addressable range emitter; composability proven byte-identical)
and **S2** (deterministic regeneration + an in-place rewrite gate).

**S3 IS BLOCKED, and the blocker is a real finding.** The S2 gate FAILS on
purpose:

    stream selfcheck: FAIL -- live span array differs from regen at span 0

The loader TRANSFORMS spans after parsing, so the live array is not what the
generator produced. That falsifies the memcpy assumption in the design: a
rewrite must either emit post-transform bytes or re-apply the loader's passes to
the rewritten region. **Next action is narrow:** find which pass mutates them.
Candidates — sentinel patching in `td5_track_bind_runtime_pointers`, the type-11
link fixup at `td5_assetsrc.c:523`, the drag geometry passes. Enable the gate
with `-DTD5RE_AUTOTRACK_STREAM_SELFCHECK` (compile-time) plus
`TD5RE_AUTOTRACK_SELFCHECK=1`.

Remaining stages: S3 live geometry, S4 route bytes, S5 scenery via runtime
display lists (`build_span_strip_display_list`, `td5_track.c:3007`, is the
seam), S6 minimap.

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
