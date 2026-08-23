# Auto-generated track: PHASE 2 STREAMING (design, not implemented)

Status: **design only.** No code written. Phase 1 (a long finite track generated
per race) is implemented and frame-verified; see `td5_trackgen.c`.

Requirement, from the original request: generate track *on the fly*, keep a
buffer covering the max view distance, and unload the parts nobody is driving
on.

## The obvious approach, and why not to take it

The naive design is to grow the span array as the player advances. It runs into
four independent blockers, all previously documented:

1. `TD5_TrackProbe.span_index` is `int16` (`td5_types.h`) — a hard ~32767 ceiling,
   so an endless road needs every probe index rebased as it grows.
2. `LEFT/RIGHT.TRK` route tables are sized to the ring at load with no realloc
   path; appending spans without route bytes is the out-of-bounds read at
   `td5_track.c:3503`.
3. The minimap builds its 256-segment table **once** at init from the whole ring
   (`td5_hud.c`).
4. AI span arithmetic wraps at ±`span_count/2` (`td5_ai.c` `smart_span_gap`),
   which sign-flips when the length changes underneath it.

## The design that dissolves three of them

**A fixed-size rolling ring buffer, presented to the engine as a CIRCUIT.**

Keep a constant N spans (N ≈ 2048). As the player advances, overwrite the spans
furthest behind with newly generated geometry. Span indices wrap modulo N —
which is exactly what a circuit track already does, and the engine's circuit
support is mature: ring-modulo arithmetic, `% ring` lap math, and the ±count/2
AI wrap are all *correct* for a fixed ring.

What that buys:

| Blocker | Outcome |
|---|---|
| 1. int16 ceiling | **Gone.** N never exceeds 2048; indices never grow. |
| 2. Route table realloc | **Gone.** Fixed size N, rewritten **in place**. |
| 4. AI ±count/2 wrap | **Gone.** Already correct for a fixed-length ring. |
| 3. Minimap | **Remains.** See below. |

It also avoids reallocating `s_span_array` at all: same pointer, same length,
contents rewritten. Runtime mutation of this array is already proven at load
time by drag mode (`td5_track_drag_apply_length`, `td5_track.c:3222`), but
in-place rewrite is strictly safer than repointing, because nothing can hold a
stale pointer.

### The lie, and the invariant that hides it

The ring is not a real loop: span N-1 is adjacent to span 0 by index but far away
in world space. That seam would be visible to anything that walks or looks across
it.

The engine only ever examines a *window*: the renderer uses ±64 spans
(`VIEW_DIST_FWD/BACK_SPANS`, `td5_render_mesh.c:2200`), and the AI and traffic
look ahead a bounded distance. So the seam is unobservable provided:

> **INVARIANT:** the write cursor stays farther from every car — in ring
> distance — than the largest look-ahead window in the engine.

With N = 2048 and the rewrite region kept roughly half a buffer behind the
player, there is ~1000 spans of margin against a ~64-span window. The invariant
must be asserted at runtime, not assumed; violating it is what would produce a
visible tear.

## What still needs solving

**Minimap (blocker 3).** Built once from the whole ring, so it would draw a fixed
loop shape that no longer matches rewritten geometry. Options, cheapest first:
(a) disable the minimap for streamed tracks; (b) rebuild the segment table when
the write cursor advances; (c) a sliding window showing only the live region.

**Scenery.** Phase 1 scenery comes from a `MODELS.DAT` parsed once at load, keyed
`entry = span>>2`. Rewritten geometry would keep the old scenery. The seam for
fixing this already exists: `build_span_strip_display_list(span_index)`
(`td5_track.c:3007`) builds a display list for one span **at runtime**, and
`:3164` already loops it over every span. Streamed scenery means calling that per
rewritten span instead of parsing a file.

**Determinism / netplay.** Generation is seeded and deterministic
(private xorshift, deliberately not the game's `rand()`), so two peers given the
same seed and the same write schedule produce identical geometry. The write
schedule must therefore be driven by *sim state* (span progress), never by
wall-clock or frame count.

**Origin blocks.** Phase 1 groups spans into 16-span origin blocks sharing an
origin, with rows shared inside a block. A rewrite must therefore operate on
whole blocks, not individual spans — the write cursor should advance in block
units.

## Staged implementation

Each stage is independently verifiable, which matters because the failure mode
here is a corrupted live track rather than a cosmetic glitch.

- **S1 — emit into a buffer at an offset.** Refactor the Phase 1 emitters so a
  run of spans can be written into an existing array at a given span offset,
  instead of only building a whole track. No game integration. Verify offline:
  regenerate a full track via S1 in pieces and byte-compare against the
  single-shot output.
- **S2 — in-place rewrite while stopped.** Rewrite a block at load time, before
  the green light, and confirm the walker, `wall_clear` and contact are unchanged.
  Proves in-place mutation is safe before anything moves.
- **S3 — live geometry streaming.** Circuit presentation, write cursor driven by
  player span progress, geometry only, no scenery. Gate: `wall_clear` sane,
  span walk monotonic, zero span jumps, invariant assertion never fires.
- **S4 — route bytes.** Rewrite `LEFT/RIGHT.TRK` rows for the same block, so AI
  has a corridor on new geometry.
- **S5 — scenery.** Runtime display lists per rewritten span.
- **S6 — minimap.** Pick one of the options above.

## Honest cost

S1–S3 is the substance and is where the risk lives. This is a multi-session
feature, not an afternoon: it touches the span array, the route tables and the
walker — the three things whose correctness took longest to establish in Phase 1,
and where every early mistake in that phase produced symptoms that looked like
something else entirely.

Recommendation: land Phase 1 first (it is coherent and verified), then do S1/S2
behind a knob with the golden-style checks above, and only then go live.
