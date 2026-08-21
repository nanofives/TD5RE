# Auto-generated track: BRANCHES (spec, not yet implemented)

Status: **researched, not implemented.** Everything below is verified against the
port's own loader and against shipped native-TD5 tracks that contain real forks
(level013, level014). Written down so implementation does not have to re-derive
it.

Branches are the last unimplemented item from the original request (straights,
curves, acute curves and dual lanes are done; see `td5_trackgen.c`).

## Why it was not implemented in the same pass as the rest

Unlike buildings/bridges/biomes, a branch is not a mesh — it changes **strip
topology**, and it collides with an invariant the generator currently depends on.

`tg_emit_strip` shares one vertex row between consecutive spans, which requires a
**uniform lane count** for the whole track. That sharing is what fixed contact
failing at every seam (before it, `wall_clear` decayed and the walker teleported
the car; after, 386/386 ticks sane). But a fork span *must* carry
`lanes = main + branch`, and its neighbours must not — so a fork cannot exist
inside a shared-row block.

Resolving that is the real work: force an origin-block boundary at the fork, and
give the widened approach run its own block(s) at the wider lane count. It is
tractable, but it is surgery on the one part of the generator whose correctness
was hardest to establish, so it wants a session with room to verify.

## Jump table

Count at `u32 @0x14`. Table starts at **`+0x18` for native TD5** (`+0x20` only
for TD6-converted, gated on `g_active_td6_level`) — `td5_track.c:3526`.

Each record is 6 bytes: `lo` u16, `hi` u16, `base` u16.

Consumer `td5_track_branch_to_main_span`, `td5_track.c:8182-8200`:

```
if (span < 0 || ring <= 0 || span <= ring || !table) return span;
for each rec: if (lo <= span && span <= hi) return span + (base - lo);
return span;
```

So `main = span - lo + base`. Pure arithmetic, **no clamping** — a wrong `base`
silently yields an out-of-ring index.

This is what keeps ring-sized `LEFT/RIGHT.TRK` lookups in bounds: `td5_track.c:6536`
normalizes actor `+0x80` into `+0x82`, and `+0x82` is what the route tables,
lap/checkpoint logic, minimap and AI all read.

## Span types

| Type | Role | Links |
|---|---|---|
| 8 `JUNCTION_FWD` | fork on the main ring, `lanes = main + branch` | `link_next` = corridor first span, `link_prev` = 0xFFFF |
| 9 `SENTINEL_START` | corridor FIRST span | `link_prev` = fork span, `link_next` = -1 |
| 10 `SENTINEL_END` | corridor LAST span | `link_next` = rejoin span, `link_prev` = -1 |
| 11 `JUNCTION_BWD` | rejoin on the main ring, `lanes = main + branch` | `link_prev` = type-10 span, `link_next` = 0xFFFF |

Types 9/10 are **drivable, not markers** — physics groups 8/9/10/11 with type 1.
They are excluded only from geometry queries and the ribbon renderer, including
`build_span_strip_display_list` (`td5_track.c:3027`), so a corridor shows two
holes if you rely on the ribbon rather than `MODELS.DAT`. Not an issue while
scenery is on, since the road mesh emits a quad per span regardless of type.

Row layout: the rail LUTs and edge masks for 8/9/10/11 are **identical to type 1**
(`td5_track.c:106,110,1315,1316,2802`), so junction spans use the plain layout —
`lvi` near row of `lanes+1`, `rvi` far row of `lanes+1`. No vertex trickery is
legal.

## Fork arithmetic

At a type-8 span F: `sub_lane < lanes(F+1)` continues to `F+1`; otherwise it
follows `link_next` with `sub_lane += lanes(link_next) - lanes(F)`
(`td5_track.c:4029-4094`). Therefore `lanes(F)` must equal
`lanes(F+1) + lanes(B0)`.

## Worked shipped example — level014

Header `[216, 2737, 92952, 23060, 3864]`, one record at `0x18`:
`(lo=2738, hi=3863, base=510)`, so branch span *s* maps to main `s - 2228`.
Fork at 509 (6 lanes = 3 main + 3 branch), main continues at 510 (3 lanes),
corridor starts at 2738 (type 9, 3 lanes, `link_prev`=509), corridor ends 3862
(type 10, `link_next`=1635), rejoin 1635 (type 11, 8 lanes).

Note the conventions: `base = fork + 1`, and `lo = ring + 1`.

## Implementation checklist

1. Widen the approach run to `main + branch` lanes, fully open for the last few
   spans before the fork.
2. Fork span F: type 8, `lanes = main + branch`, `link_next` = B0.
3. `F+1`: type 1, `lanes = main`.
4. **Emit a PAD span at index == `ring`** so the corridor can start at `ring + 1`.
5. Corridor at `ring+1 …`: B0 type 9 (`link_prev` = F), interior type 1, Bn
   type 10 (`link_next` = R). All at `lanes = branch`.
6. Rejoin R: type 11, `lanes = main + branch`, `link_prev` = Bn. Optional — the
   generated levels (041/042/045/046) omit type 11 and merge one-way.
7. Jump record `(lo=B0, hi=Bn, base=F+1)`; bump the count at `0x14`; recompute
   `span_off` / `vtx_off`.

## The three ways this goes wrong

1. **`lo == ring`.** `branch_to_main_span` rejects `span <= ring`, so the first
   corridor span is never normalized and an un-normalized index reaches the
   ring-sized route tables. **`re/tools/td5_trackgen.py` has this bug** — it sets
   `branch_start = len(spans) == ring` (`:894,:920`), confirmed in level041
   (`ring=239`, record `lo=239`). Native tracks always use `ring+1`. So the
   Python reference must NOT be copied here; insert a pad span.
2. **Lane-count mismatch at the fork** — if `lanes(F) != lanes(F+1) + lanes(B0)`,
   `sub_lane` is wrong after the fork and the car probes the wrong triangle.
3. **Zero-width outer lanes on the approach** — `sub_lane` can then never reach
   `main_lanes` and the fork is physically unreachable.

## Dead ends

Do not build one. Terminate every corridor with a type-10 whose `link_next`
points at a real main span. No shipped or generated track dead-ends, the walker's
out-of-bounds net deliberately does not fire for types 8-11, and
`td5_ai_smart_branch` has no dead-end awareness — the AI would commit to it and
strand itself.
