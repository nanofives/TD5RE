# heading_normal.y Writer Audit — actor+0x292

**Date:** 2026-05-14
**Branch:** `fix-heading-normal-y`
**Scope:** Locate the original writer of `heading_normal.y` (actor field +0x292, int16) so
the precise-port of `ApplyMissingWheelVelocityCorrection @ 0x00403EB0` (commit
aa52796) can actually fire — currently the port writes y=0, so the correction
multiplies by 0 and never fires.

## Field summary

`actor->heading_normal` is a 3 × int16 vector at actor + 0x290:

| Offset | Field          | Notes                                                       |
|--------|----------------|-------------------------------------------------------------|
| 0x290  | heading_normal.x (int16) | Read in `ApplyMissingWheelVelocityCorrection`     |
| 0x292  | heading_normal.y (int16) | Vertical / "up" component — used as a multiplier  |
| 0x294  | heading_normal.z (int16) | Read alongside .x                                 |

It is the unit normal of the track plane at the actor's location, expressed in
the int16 fixed-point grain `K = 4096` (matches `triangle_height` in
`td5_track.c`). For flat track .y ≈ 4096, for slopes it drops accordingly.

## Read sites in the original (4)

All four are pure reads (no STOREs found via `search_constants 0x292` or
`search_pcode` MOVSX/MOV r16,[r+0x292]):

| Address     | Function                                          | Use |
|-------------|---------------------------------------------------|---------------------------|
| 0x00409570  | `ApplyMissingWheelVelocityCorrection` (0x00403EB0 path) | multiplier on dot product |
| 0x004097fb  | (inside same physics block)                       | velocity correction reuse |
| 0x00409b37  | similar correction path                           | velocity correction reuse |
| 0x0040c8bb  | `IntegrateScriptedVehicleMotion` (0x00409D20 region) | scripted-motion lift     |

## Writer sites — via LEA into +0x290 then call

`search_constants 0x292` returned **zero direct stores** because the writer
receives the vector by pointer (LEA `[esi+0x290]`) and writes through it
inside a callee. Six LEA `[r+0x290]` sites exist; three of them are call-site
pointer pushes:

| Address     | Caller function                          | Callee that writes the vec |
|-------------|------------------------------------------|----------------------------|
| 0x00405fb6  | `IntegrateVehiclePoseAndContacts` (0x00405E80) | `ComputeActorHeadingFromTrackSegment` (0x00445B90) |
| 0x00406439  | `UpdateVehiclePoseFromPhysicsState` (0x004063A0) | `ComputeActorHeadingFromTrackSegment` (0x00445B90) |
| 0x00443d3d  | `UpdateTrafficVehiclePose` (0x00443CF0)  | same                                       |
| 0x00409cd1, 0x00409e00 | scripted-motion paths (0x00409D20) | computed inline                            |
| 0x00403ed2  | `ApplyMissingWheelVelocityCorrection`     | pointer pushed only as READ to a callee   |

**Primary writer for player/AI vehicles:**
`ComputeActorHeadingFromTrackSegment @ 0x00445B90` (called once per physics
tick from each of the two pose integrators).

## ComputeActorHeadingFromTrackSegment (0x00445B90) — what it does

Signature: `void __cdecl(short *track_state /*+0x80*/, int *world_pos /*+0x1FC*/, undefined2 *out_normal /*+0x290*/)`

Size: ~600 bytes, 218 instructions, two-level switch on `(span_type, end_flag)`.

The function:

1. Reads `span_idx` from `track_state[0]`, `sub_lane` from `track_state[6]`
   (= byte at actor+0x8C).
2. Fetches the span's left/right vertex base indices from
   `g_trackStripRecords` (one record = 0x18 bytes; offsets 0=type, 3=lane
   count packed in low nibble, 4=lvert base, 6=rvert base, 0xC/0x14=origin xz).
3. Computes the relative-position `local_x = (world_x>>8) - origin_x`,
   `local_z = (world_z>>8) - origin_z`.
4. Tests `sub_lane == 0` vs `sub_lane == lane_count-1` vs interior, then
   switches on the span type byte (`g_trackStripRecords[i].type & 0xF` from
   the `(&DAT_00474e40)[type*2]` lane-bias LUT lookup).
5. For each (type, edge) combination, picks a triangle (3 vertex indices)
   chosen from `{lvert, lvert+1, rvert, rvert+1, rvert+3}` and calls
   `InterpolateTrackSegmentNormal(va, vb, vc, out_normal)`.
6. The default branch is a no-op (returns without writing — leaves stale
   value).

The dispatch table at `0x00445DEC` selects 11 cases (`0..0xA`) for
sub_lane==0; another table at `0x00445E04` covers the right-edge case; the
interior branch falls through to a single triangle pick (`lvert, rvert,
rvert+1` per the `LAB_00445DD5` tail).

## InterpolateTrackSegmentNormal (0x00445E30) — the actual STORE

~217 bytes, ~60 instructions. Computes:

```
e1 = vb - va    (int32 from int16 vertices, sign-extended)
e2 = vc - va
n  = CrossProduct3i(e1, e2)        // int32 cross
n >>= 12                            // SAR each component by 12
ConvertFloatVec3ToShortAnglesB(n, out_normal)
                                    // FPU normalize to length 4096,
                                    // __ftol truncate to int16
if (out_normal[1] == 0) out_normal[1] = 1
```

Writes 3 × int16 to actor+0x290..0x295. Step 5 (the post-conversion
`if (uny == 0) uny = 1` guard) is the reason the y component is never exactly
zero in the original — it is the **minimum sentinel** that lets the
`ApplyMissingWheelVelocityCorrection` divide-by-y path stay finite. The port's
`triangle_height` helper in `td5_track.c:2832` already implements steps 1-6
correctly, but only for *suspension* probes — that result is not stored to
+0x290.

## Helper dependencies (port status)

| Original helper                       | Address     | Port has it? | Where                       |
|---------------------------------------|-------------|--------------|-----------------------------|
| `CrossProduct3i`                      | 0x0042EA70  | Yes (inlined in `triangle_height`) | `td5_track.c:2858-2860` |
| `ConvertFloatVec3ToShortAnglesB`      | 0x0042CD40  | Yes (inlined) | same block                  |
| `__ftol` (truncate-toward-zero)       | runtime     | Implicit C cast | int16-cast of double      |

So no new helper has to be ported — `triangle_height`'s existing math IS the
inner `InterpolateTrackSegmentNormal`. The remaining work is the
**vertex-pair dispatch glue** of `0x00445B90`.

## Existing port-side substitute

`td5mod/src/td5re/td5_track.c:3139 td5_track_compute_heading()` currently
maps to `InitializeActorTrackPose @ 0x00434350` (the **spawn-only** initial
pose), NOT `ComputeActorHeadingFromTrackSegment @ 0x00445B90`. It writes:

```c
heading_normal[0] = dx;     // 24.8 edge difference
heading_normal[1] = 0;       // <-- hard-coded zero
heading_normal[2] = dz;
```

`td5_physics.c:5957` calls this helper from `UpdateVehiclePoseFromPhysicsState`
with an inline TODO acknowledging the substitution. Same call exists in
`IntegrateVehiclePoseAndContacts`. The consequence is that every read site
that multiplies by `heading_normal.y` (offset 0x292) gets a zero, neutering
the correction.

The td5_track.c comment block at 3091 already references both `0x435CE0`
(InitializeActorTrackPose for some chassis-snap path) and `0x445B90`, but
the implementation matches only the former.

## Suggested port plan (≥40 instructions — separate worktree)

1. Add a new helper, e.g. `static void td5_track_runtime_heading_normal(TD5_Actor *actor, int16_t out[3])`,
   in `td5_track.c`, located next to `td5_track_compute_heading`.
2. Translate the dispatch table at `0x00445DEC` / `0x00445E04` to a switch.
   The 11 cases correspond to span type values 0..0xA where:
   - **default / no-op**: types whose dispatch table entry is the
     `switchD_00445c37_default` label.
   - **interior (sub_lane in middle)**: pick `(lvert, rvert, rvert+1)`
     after diagonal-cross test.
   - **left edge (sub_lane==0)**:
     - types 1,2,5,8,9,0xA,0xB: diagonal cross then `(lvert, lvert+1, rvert+1)` or `(lvert, rvert, rvert+1)`.
     - types 3,4: `(lvert+1, rvert, rvert+1)`.
     - types 6,7: `(lvert, rvert+1, lvert+1)`.
   - **right edge (sub_lane==lane_count-1)**:
     - types 1,3,6,8,9,0xA,0xB: diagonal cross dispatch into
       `(lvert, lvert+1, rvert+1)` or fall-through.
     - types 2,4: fall-through (interior triangle).
     - types 5,7: `(lvert, rvert, lvert+1)`.
3. Re-use `triangle_height`'s cross-and-normalize math, but DROP the
   barycentric height step (we only need the unit normal). Easiest path:
   factor `triangle_height`'s steps 1-5 into a new
   `compute_triangle_unit_normal(va,vb,vc,out)` and call it from both.
4. Wire it in at `td5_physics.c:5957` (replace the
   `td5_track_compute_heading(actor)` line) and at the second pose function
   (`IntegrateVehiclePoseAndContacts`), plus the traffic pose call.
5. Verify on flat track that `heading_normal.y` ≈ 4096; on sloped track
   that |.y| < 4096 and matches the slope cos.
6. Re-run the chassis-snap/vy-correction pilot trace to confirm
   `ApplyMissingWheelVelocityCorrection` now produces non-zero corrections
   on slope onsets.

## Direct deliverable for this audit

The full port is ≥218 (dispatch) + ≥60 (inner) instructions = far over the
40-instruction threshold for an in-place precise port. Per protocol, this
worktree commits only this audit. A follow-up agent should pick up the plan
above into a `precise-00445B90` worktree.

## Files touched

- `re/analysis/heading_normal_y_writer_audit.md` (this file) — new.

## Cross-references

- `td5_physics.c:5944` — TODO comment acknowledging substitution.
- `td5_physics.c:6869` — `ApplyMissingWheelVelocityCorrection` reads
  `hn_p[1]` = heading_normal.y.
- `td5_track.c:2832-2900` — `triangle_height` already has the unit-normal
  math; factor + reuse.
- `td5_track.c:3139` — `td5_track_compute_heading` (currently maps to wrong
  original function).
