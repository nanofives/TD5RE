# Autotrack feedback round 21 (2026-09-08)

Unlike rounds 2-20 this one is not a manual-drive defect list. It is a
**feature request**: make the AUTO TRACK STUDIO produce variety by default
instead of asking the player to set 46 fixed values, and fix the one row that
could not do its job.

Branch `feat/autotrack-r21-random`, based on master `a6133237`. Measurement
seed **5150** throughout (the same pinned card the feedback harness uses), with
777 / 99991 / 20260908 as the boxed-in cross-check.

Numbered **R21** and not R19: master already carried R18, an R19 and an R20 by
the time this started.

## Verbatim items

1. remove lane selector, during creation you should be able to choose sections
   where there's narrower lanes and wider lanes
2. twistiness should be random as well, avoid a lot of twistiness on cities,
   each biome should have its own twistiness
3. dual lanes should be random as well, avoid having a lot of assymetrical dual
   lanes (it can happen, just not that often)
4. corners should be random as well, cities and have more tight corners, forest
   can be normal
5. steepness is not making very steep climbs
6. i want most of the knobs to be actually not toggleable, i want as random as
   it can get on most things
7. also i want you to set the infrastructure for more things i want to add to
   the generator

## Two premises in the request that measurement contradicted

Both are recorded because acting on either as stated would have produced a
change that did nothing.

### Item 5: the GRADIENT row is a CAP, so it cannot make a climb

Measured before writing any code, seed 5150:

| knob | rescale fired? | worst grade | RANGE |
|---|---|---|---|
| defaults | **no** | 0.1199 (cap 0.119) | 34007 |
| `GRADE=200` | **no** | 0.1279 (cap 0.199) | **34007** |

STANDARD to SEVERE changed the height range by **nothing**. A cap can only ever
reduce `|dy|`; nothing in the profile drove slope toward it. The pre-fix p90
slope was 0.0623 -- barely half the cap -- which is precisely why nothing felt
steep. The 0.008 that did move came from the bridge crown budget being
unclamped, not from terrain, which is also why the two MODELS.DAT files are the
same SIZE with different hashes.

The obvious suspect was wrong too: the global max-norm rescale was **not** the
cause. It only fires when HILLS is raised, and the new log prints `the old
global rescale would have been x1.000` on this seed. **Grep race.log for
`rescaled` before blaming it.**

### Item 1: per-section narrower/wider lanes already existed

`TD5RE_AUTOTRACK_LANE_VARY` is default ON in master, with `LANES_MIN 2 /
LANES_MAX 8 / LANE_PCT 35`, seam transitions and half-lane jog compensation.
The reporting tree was ~40 commits behind master, which is the likely reason it
had not been seen. So item 1 reduced to: delete the row that set one count for
the whole track, bias the existing variation by biome, and make its rate rolled.

## What shipped

### The roll registry (items 2, 3, 4, 6, 7)

47 entries, one per studio option row. Each resolves as *pinned by a knob* or
*derived from the seed*, latched once per build before the centerline walk.

- Placed **above `tg_rand()`** so a roll is mechanically unable to consume the
  geometry RNG. Rolls hash instead, the same discipline `tg_biome_hash` states.
- Keyed by a private **salt** with no table-index term, so entries can be
  appended or retired without moving any other entry's roll -- and therefore
  without changing any existing seed. A duplicate-salt scan logs the one
  mistake that would silently destroy variety.
- Folded into the spec **before the GENSTAMP is hashed**, so a different roll is
  a different `spec_hash` and the build cache can never serve a track that does
  not match the seed. That is why `TG_STAMP_VERSION` did not need a bump.
- The `[R21 ROLL]` block is emitted **before** the reuse early-return, so a
  reused build -- which prints no inventory at all -- still says what it is.

**Environment publishing** avoided 40 call-site edits: around 40 of these knobs
are read by `td5_env_*` calls spread across the eight generator modules, so the
registry writes each resolved value into the environment and every existing read
picks it up. Ownership is the whole trick -- a published value is
indistinguishable from a pin once `getenv` sees it, so the registry records what
it wrote and gives it back before the next resolve. Without that, the first
build's roll would look pinned forever and the studio row would stop showing
RANDOM after one race. TIME OF DAY is never published: its knob is a tri-state
where 2 means "roll it", so publishing 0 or 1 would freeze it for good.

**STYLE vs PRESENCE.** 23 entries carry real weights. The other 23 remove
content when off (guardrails, sidewalks, scenery, crossings, bridges); a seed
that rolled several of those off at once would read as broken rather than
varied, so they are weighted to today's value while staying in the mechanism
and in the report -- a later round tunes one byte. LENGTH is weighted away from
both ends because a rolled 3000-span MARATHON triples build time on a row nobody
touched.

### Per-biome road character (items 2, 4)

`k_biome_road`, parallel to `k_biomes` rather than nine more fields on it --
those rows are already ~28 positional initializers and C diagnoses nothing if
one lands in the wrong column. Every field is a **percent of what the studio
configured**, so TWISTY still gives a twisty track whose cities are its *least*
twisty part rather than a table that overruled the row.

```
   name          str  cur  acu dual  corn  wid  var  rlf  grd  blk
   CITY          140   70  110   60    70   85   10   60   80   16
   FIELDS        120  110   60  130   130  105   20   90   90    0
   FOREST        100  100  100  100   100  100   15  110  110    0
   INDUSTRIAL    130   80   80  140   110  110   20   70   80    0
   ALPINE         60  120  170   40    75   85   10  180  150    0
   COAST          90  140   90  110   110  100   15  100  100    0
   ORIENTAL      100  110  110   70    90   90   15  110  105    0
   ALPTOWN       110  100  120   60    80   85   10  140  130   24
```

CITY's two clauses are not in tension once separated: `str 140 / cur 70` means
**fewer** turns (grid streets are straight lines) while `acu 110 / corn 70`
means the turns it has are **sharp block corners**. FOREST is the all-100
identity row -- "forest can be normal", and the neutrality check.

Corner tightness reaches its three readers as three *different* quantities:
`tg_adjacent_skip` keeps a track-wide worst case (a min over the whole table,
not over the biomes this seed laid out); the ACUTE radius is per-section; the
curvature floor is per-**span**, because a section can straddle a cell boundary.
Scalars ramp across run edges via `tg_biome_run_bounds` -- never
`tg_biome_for_span`, which dithers per span and would flip a corner floor
between two biomes on alternating spans.

### The gradient drive (item 5)

A **gain** stage, with the cap kept as the safety bound:

1. **Drive** -- measure the profile's p90 per-span slope and scale the whole
   profile so p90 lands on the target. p90 and not the max: keying on the max is
   what the old rescale did, and one freak span then set the scale for the
   entire track.
2. **Soft limit** -- clip each span with a `tanh` knee against a per-span cap.
   `tanh < 1` gives `|dy| < cap` always, so the bound is a theorem rather than
   something a global rescale enforces afterwards. Spans under the knee are
   bit-unchanged, and value and first derivative match at the knee.
3. **Per-biome** -- the cap carries `grade_pct`, ramped across run edges, so
   ALPINE keeps steep pitches where CITY is clipped gentle. Applied to the
   **cap** and not the gain, because the limiter is already smooth per span
   whereas a per-span gain on `y` would step at every boundary.

The limiter differences the *original* profile and integrates the clipped
result, carrying both previous input and previous output; differencing in place
would feed each clipped value back in and drag the whole tail down.

| seed 5150 | p90 | RANGE | ascent | longest steep run |
|---|---|---|---|---|
| before | 0.0623 | 34007 | — | — |
| STANDARD + drive | 0.1177 | 65430 | 75300 | 128 spans |
| SEVERE + drive | 0.1856 | 108543 | 120823 | 154 spans |

STANDARD to SEVERE now moves RANGE **+66%** where it used to move it 0%.

### Lane variety (item 1)

The LANES row is deleted. The count each section's random walk reverts toward is
now the biome's own typical width -- narrow city streets, wide highway biomes --
same draws in the same order, only the comparison target moves. LANE VARIETY
(how often it changes) replaces it and is rolled.

Also clamps `LANES_MAX` to 10: the real width ceiling is the **int16 vertex
offset**, not the vertex count. At block 16 and span_length 1500 the budget is
`24000 + width/2 < 32767`, so 12 lanes (18000) fails the whole build on a hard
check about vertex offsets. Default 8 is unaffected.

### The studio (item 6)

RANDOM is a **virtual index 0** on every registry-backed row, concrete choices
shifting to 1..n -- rather than prepending -2 to a dozen value tables and
bumping a dozen `n` fields, which is the same change spread over far more places
that can each go wrong. `roll_id` is resolved from the knob *name* at runtime,
so the row table needs no parallel id column to keep in step.

The knob's spelling of RANDOM is **absence**, which is what keeps the rest
consistent: `td5_env_*` treats unset as its default, `at_fav_capture` omits
unset knobs, and `tg_env_hash` skips absent names, so a row at RANDOM hashes
identically to one never touched and the build cache still hits.

Values are left-aligned and marked with a leading `~` plus a tint. Both layout
defects here were found by framedump, not by reading: centring only worked while
values were short words (`RANDOM (EXTREME)` centred at 258 lands on the button
labels), and the first legend placement was clipped off the bottom of the frame.

Three self-checks that were previously only *claimed* in a comment now actually
run at screen entry. One of them immediately caught a real bug: TIME OF DAY
already had its own RANDOM value, so the virtual one listed it twice.

**Favourites migration** had to land in the same commit as the row change. A
favourite saved before R21 also omitted unset knobs, but omission then meant the
shipped constant -- replaying one would silently recall a different track.
Capture writes an `R21=1` marker; apply prefills the legacy constants when it is
absent. The replay loop is also restricted to the `TD5RE_` namespace, so a
hand-edited progress ini cannot set arbitrary environment variables.

### Infrastructure (item 7)

Landed wired but inert, so the next round adds a table row and not a mechanism.

- **MOOD** (season, weather, wetness, wear, grip, fog) resolved, latched and
  logged, consumed by nothing. Wetness and fog are *derived* from weather, not
  rolled independently, because a dry CLEAR track at 80% wetness would be
  incoherent. `grip_pct` carries a **floor**, not a free roll: grip reaches the
  simulation, and the precedent is ALPTOWN's tarmac cap, which exists so "the
  race still finishes" keeps holding.
- **LANDMARKS**, a placement registry for one-per-track set pieces. Walks
  **merged** biome runs, because with repeated cells a 300-span city is one run
  where `si / TD5_TG_BIOME_RUN` sees two and could offer the same city a
  once-per-track piece twice. Hash-gated, runs after elevation (so `needs_flat`
  is evaluable) and before scenery.
- **Biome asserts.** Adding a biome is now a documented procedure with the trap
  as a build error: the two COUNTs are draw *moduli* while `TD5_TG_BIOME_KINDS`
  is the array *bound*, and a COUNT larger than the table reads past it on every
  cell drawing the missing biome.

Deliberately **not** done: replacing the two moduli with explicit draw-pool
arrays. The mapping today is the identity, so it would change nothing while
putting every seed's biome layout at risk.

## Not delivered as asked

**Asymmetric dual carriageways (item 3)** ship as *appearance*, not geometry.
The strip emitter writes every vertex row symmetrically about the node centre,
so true asymmetry needs a `TG_Node.lat` field plus an audit of every
`width * 0.5` consumer -- half-width, fork gore, sidewalks, verges, guardrails,
the AI route lateral, the span walker. That is its own round. The dual-lane
*rate* is rolled, and the existing avenue divider is the asymmetric-reading
element.

**Roundabouts**, raised under item 7's future shapes, are not expressible as a
section kind at all. The walk's non-trapping proof requires positive forward
progress along `TD5_TG_AXIS_HEADING`, which is why even a 180-degree hairpin is
deliberately refused. A roundabout has to be scenery plus a junction.

## Knobs

| knob | default | effect |
|---|---|---|
| `TD5RE_R21_ROLL` | ON | the whole registry; 0 makes every unpinned entry take its pre-R21 constant |
| `TD5RE_R21_ROAD_CHAR` | ON | per-biome section mix, corner tightness, lane aim, grade cap |
| `TD5RE_R21_GRADE` | ON | the gradient drive and soft limiter; 0 restores cap-only + global rescale |

All three at 0 is **byte-identical** to pre-R21.

## Measurements

Seed 5150, one knob at a time, `level090` deleted before every run:

| config | MODELS.DAT | sha256 |
|---|---|---|
| pre-R21 baseline | 8,617,068 | `032CCEC3` |
| `R21_ROLL=0` | 8,617,068 | `032CCEC3` (identical) |
| all three knobs 0 | 8,617,068 | `032CCEC3` (identical) |
| `ROAD_CHAR=1` alone | 8,602,588 | — |
| shipped defaults | 8,604,976 | `2F7AC6E5` |
| defaults, repeated | 8,604,976 | `2F7AC6E5` (deterministic) |

`adjacent_skip` 16 to 17 under `ROAD_CHAR=1` -- the predicted **rise**, since a
tighter arc can double back in fewer spans so provable separation needs more.
That prediction is written into the code comment: if it had not moved, the
worst-case path was not wired up.

Boxed-in cross-check, all knobs on: seeds 5150 / 777 / 99991 / 20260908 gave
2069 / 2669 / 2069 / 2069 spans, **no "boxed in"** on any, output 8.6-13.9 MB.

## Not verified

Everything above is generation metrics, logs and two framedumps of the studio
screen. **No lap has been driven.** The three things to look at first:

1. A CITY stretch -- long straights with hard 90-degree block corners, not
   sweeping curves.
2. An ALPINE stretch -- the steep climbs the round exists for, and whether
   ~0.19 grade is drivable rather than merely steep.
3. City crossings, zebras and medians at the **narrow** end of the new lane aim.

The `[R21 GRADE]` line reports p50/p90/p99, the per-biome cap range, limiter
hits and the longest steep run; **p90 is the number to judge it on**, because
worst grade was pinned at the cap before and is pinned just under it after.
