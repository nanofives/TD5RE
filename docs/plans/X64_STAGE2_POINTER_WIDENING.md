# x64 Stage 2 — pointer widening (still 32-bit)

Status: **SCOPED, NOT STARTED** (2026-07-28)

## Context

DXR requires a 64-bit process (measured: RTX 5070 Ti reports RaytracingTier **0** in 32-bit, **12**
in 64-bit, WARP reads 1.1 in both as control). The x64 retarget is staged so only ONE stage breaks
the golden traces:

- **Stage 0** (`d3d191ce`) — layout `_Static_assert`s. DONE.
- **Stage 1a** (`4b732465`) — x87 trig LUT → `lrintf`, bit-identical. DONE.
- **Stage 1b** (`323ffb8c`) — SSE2 float math, goldens re-recorded. DONE, **merge-gated** on a
  verification run blocked by a pre-existing `nvwgf2um.dll` crash.
- **Stage 2** — THIS PLAN. Pointer widening, still 32-bit.
- **Stage 3** — flip `-m32` → `-m64`. Build-side only, ~half a day.

**The defining property of Stage 2: pointer width does not change arithmetic, so the golden traces
must stay GREEN throughout.** They are the proof the rework is correct. Any golden movement during
Stage 2 means a real bug was introduced. This is why Stage 1b was sequenced first — to get the FP
change out of the way while the net could still be trusted.

## Decision: mesh headers use option C (split disk/runtime)

Three options were evaluated against the code. Verified findings:

- **The rebase base is the mesh header itself** — `td5_track_parser.c:440` is literally
  `base = (uint8_t *)mesh;`. Offsets are mesh-relative, so the base is in scope at 100% of read
  sites. (The comment at `td5_track_parser.c:444-446` claims the port stores these "as
  uintptr_t-compatible values" — false; they are `uint32_t`. That comment is the bug, written down.)
- **(A) widen fields in place — REJECTED.** Breaks ~17 `sizeof(TD5_MeshHeader)`-against-disk checks
  (`td5_asset.c:3647/3930/4003`, `td5_track_parser.c:265/268`, `td5_track.c:7129/7358/7522/8421`,
  …) plus the TD6 emitter layout at `td5_asset.c:3339-3371`. The struct is genuinely a disk mirror.
- **(B) never rebase, pure offsets — REJECTED.** Cannot represent 2 of 6 producers:
  `td5_track.c:7608` writes `(uintptr_t)nn | 1u` where `nn` is an independent `calloc` (and bit 0 is
  a DERIVED tag); `td5_render_mesh.c:1525` stores a pointer into the per-pane vertex workspace.
  Also 8 of 16 `vertex_data_ptr` sites have no mesh in scope (the `dispatch_*` handlers take only
  `(cmd, base_verts)` — `td5_render_internal.h:502`).
- **(C) split disk mirror + runtime struct — CHOSEN.** Nothing structurally broken, and it is
  **half-built already**: `TD5_TrackRawMeshHeader` (`td5_track_internal.h:20-39`) is an existing
  `#pragma pack(1)` mirror with identical layout.

Only **6 places populate** these fields (the conversion surface): `td5_track_parser.c:447-452`
(the one rebase, called from 5 sites), `td5_asset.c:3369-3379`, `td5_track.c:304-311`,
`td5_track.c:2799-2859`, `td5_track.c:7527-7608`, `td5_render_mesh.c:1525`.

Also fixed by C, and easy to miss: **mesh pointers are themselves stored in `uint32_t` slots written
back into the file blob** — `td5_track_parser.c:274` (`*slot = (uint32_t)(uintptr_t)mesh;`),
`td5_track.c:2793`, read at `td5_track.c:7128/7357/7518`, `td5_render.c:1160/3682`.

**Must revisit ~13 validity heuristics** that encode the pointer-vs-offset assumption, notably the
explicit tri-state at `td5_render_mesh.c:1496-1503` and `td5_track.c:7150-7151`
("0 => sequential cursor, small => mesh+offset, large => absolute pointer"), the `< 0x10000u`
tests, `td5_track_is_ptr_in_blob` / `td5_track_is_valid_mesh_ptr`, and the bit-0 DERIVED tag on
`normals_offset` (should become a separate flag under C).

## Mesh work: increments, and a rejected first attempt (2026-07-28)

Two corrections to the analysis above, both found by reading the code rather than surveying it.

**1. `TD5_TrackRawMeshHeader` is NOT a drop-in disk mirror.** It and `TD5_MeshHeader` are the
same 0x38 bytes read DIFFERENTLY: raw opens `uint8_t render_type; uint8_t texture_page_id;
uint16_t flags`, while `TD5_MeshHeader` opens `int16_t render_type; int16_t texture_page_id`.
"The split is half-done" overstates it — the existing type is a second *view*, not the runtime
half of a split.

**2. Meshes live in the file blob for their whole life.** `td5_track_parser.c:266-269` casts
`block_base + mesh_off` in place, and `:274` writes `*slot = (uint32_t)(uintptr_t)mesh` — a
runtime pointer stored back INTO the blob, over what was file data. There is no separate runtime
allocation to migrate to; option C must create one.

### Rejected: "side table of mesh pointers keyed by (dl, j)"

The obvious first increment — stop writing pointers into the blob, keep a parallel runtime array
indexed by display-list and slot — **does not work**, and the reason matters:

`td5_render.c:3641-3644` SYNTHESIZES a display-list block on the stack:

```c
fake[0] = 1;
fake[1] = (uint32_t)(uintptr_t)g;   /* 1-entry block: just the gantry */
td5_render_span_display_list((void *)fake);
```

That block is not in the models blob and has **no `(dl, j)` identity**, so a side table keyed on
those cannot represent it. `td5_render_span_display_list` is a shared consumer taking BOTH
blob-resident and stack-synthesized blocks.

**The slot type is therefore a real interface, not an implementation detail.**

Also: the reader surface spans THREE idioms, so grep-by-one-pattern under-counts it —
- `*(uint32_t *)(block_base + 4 + j * 4)` — `td5_track_parser.c:255/329/349/488`,
  `td5_track.c:7121/7355/7518`
- `block[j + 1]` array indexing — `td5_render.c:1159`
- the synthesized producer above — `td5_render.c:3642`

Had the side table been implemented against only the first idiom, the drag-gantry path would have
silently rendered nothing: a visual regression the TRACE goldens cannot catch, and the RENDER
goldens probably would not either, since neither golden scenario is a drag race.

### Revised increments

1. ✅ **DONE `f48b2e11`** — **Change what a display-list block IS**: `TD5_SpanDisplayList
   {count, TD5_MeshHeader **meshes}` (`td5_types.h`) instead of a `uint32_t[]` of truncated
   pointers. The blob KEEPS its `uint32_t` layout — it cannot change, because MODELS.DAT format
   autodetection (`td5_track_parser.c:99-100/152-157`) uses "first DWORD in [1,256]" as the
   DEFINITION of a valid block — so truncation is now confined to parse time.

   **The surface was bigger than this plan recorded. There are FIVE idioms, not three**, and a
   grep for the documented `4 + j*4` shape finds neither of the two that were missing:
   - **bare `+4` first-entry reads** — `td5_track.c:404` (pre-relocation, an OFFSET) and
     `:2464-2465` (post-relocation, a POINTER). No `j`, so the documented grep misses them.
   - **`TD5_FallbackDisplayList`** (`td5_track.c:220`) — a C struct whose first two fields ARE a
     one-entry block, handed out as `void *`. A third representation the plan never mentioned.

   Three constraints that would each have caused a silent regression:
   - **Blocks are deduped by POINTER** (`s_submitted[]`, the munich-gantry-double-submit fix) and
     the drag tiling loops reuse the SAME block as an immutable template while mutating a global
     z-offset ⇒ blocks must be stable cached objects, never per-call temporaries.
   - **Generated blocks fuse header and payload in ONE allocation** (`free_display_lists` frees
     them with a single `free()`), and the payload offset was a baked `sizeof(uint32_t) * 2` —
     now derived, or it silently mis-places the mesh when the header changes width.
   - **Build the runtime table AFTER the post-relocation sweep**, which is still zeroing bad
     slots; building inside the relocation loop captures meshes the renderer then rejects.

   Verified: suite 55/55, all three trace goldens green (`race-golden-drag` included — it exists
   for exactly this path), `rgold-race-golden-drag` worst L1=65 vs a 4000 limit.
2. Runtime header array + parse-time conversion; consumers still read the old fields.
3. Repoint the 34 header consumers, then the 16 `vertex_data_ptr` sites (needs the
   `PrimDispatchFn` signature change for the 8 dispatch handlers with no mesh in scope).
4. Revisit the ~13 validity heuristics — the `< 0x10000` tri-state and the bit-0 DERIVED tag.

**Testing gap to close first:** no golden scenario is a drag race, so the drag/gantry path is
unguarded. Consider adding one before increment 1, or accept manual verification for it.

## MEASURED x64 breakage — trial compile, 2026-07-28

The remaining work is no longer estimated. A portable x86_64 MinGW-w64 was placed at
`td5mod/deps/mingw64/` (gitignored) and every module in `srcs.txt` was compiled `-c` for x64.

**The toolchain is GCC 16.1.0 — the SAME version as the bundled i686 one, same packager
(winlibs / Brecht Sanders).** That matters: every diagnostic below is an ARCHITECTURE effect,
with zero compiler-version noise. Compile-only means no linking, so no x64 zlib or import libs
are needed (zlib's two headers are arch-neutral and can be staged from the i686 sysroot).

### Result: 45 of 67 modules already compile clean on x86_64

22 failing modules, 445 errors, and they reduce to exactly **two causes**:

| Cause | Errors | Status |
|---|---|---|
| `TD5_Actor` layout grows | 435 (37 distinct asserts) | the whole job |
| `main.c` CONTEXT registers | 10 | **FIXED** |

**No truncating-pointer errors at all.** `-Werror=int-conversion` and
`-Werror=incompatible-pointer-types` were active and found NOTHING — the pointer work already
committed (route/script handles, camera `int actor` API, `g_actorBaseAddr`, span/vertex tables)
cleared that entire class.

### The single root cause

`sizeof(TD5_Actor)` grows 0x388 -> 0x398: the four `void*` at +0x1B0..+0x1BC gain 16 bytes
total, shifting every field from +0x1B0 onward. That one fact produces all 435 failures:

- `TD5_Actor size drifted from 0x388`
- 18 struct-field asserts in `re/include/td5_actor_struct.h` (world_pos, display_angles,
  steering_command, slot_index, race_position, euler_accum, ...)
- 18 AI cross-check asserts in `td5_ai_internal.h` (CAR_DEF_PTR, LIN_VEL_X/Z, WORLD_POS_X/Z,
  STEERING_CMD, THROTTLE_STATE, SLOT_INDEX, ...)

**Confirmation the boundary is exactly as predicted: NOT ONE assert below +0x1B0 fired.** The 8
AI constants documented as "safe" (span + probe offsets) all passed. The tripwires did precisely
what they were added for — the compiler now hands over the worklist instead of it being
discovered by debugging wrong AI behaviour.

### What the compiler CANNOT find

The mesh-header `uint32_t` offset fields produced **zero** errors, because storing a truncated
pointer in a `uint32_t` is valid C on x86_64 — it fails silently at RUNTIME, above 4 GB. That is
why the mesh work needs the golden net (and why the drag golden was added), and it is the one
remaining area where "it compiles" proves nothing.

### Reproducing

`scratchpad/x64_trial.ps1` drives it: stage zlib headers, compile each `srcs.txt` entry with
cflags minus `-m32/-msse2/-mfpmath=sse` plus `-m64`, keep the `-Werror=` classes (they ARE the
pointer-truncation detectors), group the diagnostics. Rerun it after each Stage 2 step to watch
the list shrink. It needs nothing to RUN, so it is immune to the `nvwgf2um.dll` fault that
blocks golden verification.

## Work areas

| # | Area | Sites | Class |
|---|------|-------|-------|
| 1 | Mesh headers (option C) | ~63 code sites, 8 modules; 6 producers | DESIGN |
| 2 | AI route-state pointers | ~40 | **DESIGN** |
| 3 | Camera `int actor` API | 10 signatures + 30 call sites | **DESIGN** |
| 4 | Actor stride | 60 | MECHANICAL (except save) |
| 5 | Actor field offsets | 476 macro + ad-hoc (count uncertain) | MECHANICAL via `offsetof` |
| 6 | `int` globals | 3 globals, ~16 sites | Mixed |
| 7 | `td5_save.c` serialization stride | 6 | ✅ DONE — field-mapped |
| 8 | Wrapper M2DX patch | 2 | Non-issue — `#ifdef` out |

## Two bugs that exist TODAY (fix regardless of x64)

1. **`td5_ai.c:4004-4014`** — script-program rotation compares a sign-extended `int32_t` slot
   against full pointers. Works on 32-bit by luck; on x64 it can never match and the rotation
   **silently stops** with no crash. Verified by reading the code.
2. **`td5_camera.c:2984/2987`** — passes `(int)((char *)&g_actorBaseAddr + slot * 0x388)`, the
   ADDRESS OF the global rather than its value. Already wrong; survives only because
   `LoadCameraPresetForView` dereferences conditionally and these callers pass `force_reload=0`.

## Recommended order

1. **`td5_ai.c:4004`** → replace the two RS pointer slots (`RS_ROUTE_TABLE_PTR`,
   `RS_SCRIPT_BASE_PTR`) with small **integer handles** (route id 0/1, program id 0-4) indexing side
   tables. This fixes the latent bug today, keeps the `int32_t` RS array and its 0x47 stride intact
   (so the trace/golden layout is unchanged — important, `td5_control.c` and
   `tools/diff_replay_frames.py:173,177` treat those slots as trace-visible), and converts ~40
   truncating sites into int compares. Strictly better than widening the array.
2. **Camera `int actor` API** → change the 10 signatures to `TD5_Actor *` (or `uint8_t *`), fix the
   30 call sites, and fix the address-of bug above.
3. **Delete `g_actorBaseAddr`** — it is redundant. Correctly-typed parallel globals already exist
   (`g_actor_pool` / `g_actor_base` / `g_actor_table_base`, `td5_game.c:82-84`, set together at
   `:3472-3474`). Only two readers, `td5_camera.c:4347/4373`; point them at `g_actor_table_base`.
   Cheapest fix in the whole stage.
4. **`g_spanTable` / `g_vertexTable`** (`td5_camera.c:127-128`) → retype to `const uint8_t *` and
   fix the 10 address-arithmetic sites. Wide but no semantic decisions.
5. **Mesh headers, option C** — the bulk.
6. **`offsetof` sweep** — repoint every `ACTOR_*` / `ACTOR_OFF_*` constant at
   `offsetof(TD5_Actor, …)`, and `#define TD5_ACTOR_STRIDE sizeof(TD5_Actor)`. The 476 macro-based
   raw accesses route through `actor_ptr()` (`td5_ai_internal.h:104`) and need no change once the
   constants are correct.
7. **`td5_save.c` stride** — see open question below.

## RESOLVED: `td5_save.c` stride — field-mapped (2026-07-28)

**The "truncate vs field-map" framing rested on a false premise.** Truncating was described as
"acceptable for a retired format", on the assumption that a frozen 0x388 stride preserves existing
saves. It does not: **the 16-byte growth is INTERIOR, at +0x1B0 — not a tail.** A 0x388-byte blit
of an x86_64 actor is a *different layout that merely has the same size*, so legacy `CupData.td5`
files would deserialize into shifted garbage. Worse, the same-build round-trip self-test would keep
passing throughout, because it saves and loads with the identical wrong layout. Silent failure.

Field-mapping is therefore the only option that delivers the compatibility the frozen stride exists
for. Implemented as `actor_save_pack` / `actor_save_unpack`, and it is **three regions, not a
per-field enumeration of ~900 bytes**, because the single interior gap is the whole difference:

| region | disk | live (i686) | live (x86_64) |
|---|---|---|---|
| head | `[0, 0x1B0)` | same | same |
| pointer block | 16 bytes | `0x1B0..0x1C0` | `0x1B0..0x1D0` |
| tail | `[0x1C0, 0x388)` | `0x1C0..0x388` | `0x1D0..0x398` |

Tail length is `0x1C8` on BOTH arches (`0x388-0x1C0 == 0x398-0x1D0`). The live tail offset is
derived via `offsetof(TD5_Actor, angular_velocity_roll)`, which is what makes one body correct on
both arches with no `#ifdef`.

**The pointer block is not serialized in either direction.** Those are process-local addresses,
meaningless across a save/load boundary by construction; `InitializeRaceActor` re-fills them and
+0x1B4 is documented dead-vestigial. This is the one intentional behaviour change: the old code
scrubbed those slots only on the extended-overlay path, leaving the LEGACY path to restore dangling
pointers into live actors — exactly what that scrub's own comment says it wants to prevent.

Verified two ways, neither of them an argument:
- **x86_64**: compiling `td5_save.c` alone for `-m64` produced 19 assertion failures, ALL from
  `td5_actor_struct.h` (the known root cause) and **zero from `td5_save.c`** — so the new asserts,
  including `TD5_ACTOR_LIVE_TAIL_OFF + 0x1C8 == sizeof(TD5_Actor)` (`0x1D0 + 0x1C8 == 0x398`), hold
  under the grown layout.
- **i686**: full suite 55/55 PASS, `save-load-roundtrip` green and all three trace goldens matching
  — the change is inert on the current build, which is why the goldens can prove it.

## Verification

- **Goldens must stay GREEN at every step.** Pointer width does not change arithmetic; movement =
  regression. Run `pwsh scripts/selftest.ps1 -Suite full` after each work area, with a pristine
  `td5re.ini` (the working copy carries Difficulty/Dynamics changes that cause false failures).
- The **33 `_Static_assert`s** in `re/include/td5_actor_struct.h:682-716` will fail loudly on the
  eventual x64 rebuild for every offset >= 0x1B0 — that is the automatic worklist for Stage 3, not
  a problem. Stage 0's asserts on `TD5_MeshHeader`/`TD5_PrimitiveCmd`/wire structs do the same for
  area 1.
- Blocked, as of 2026-07-28, by the same `nvwgf2um.dll` crash gating Stage 1b.

## Uncertainties (do not treat as settled)

- Ad-hoc raw `*(T*)(ptr + 0xNN)` actor-offset counts in area 5 — the survey used a regex that
  over-counted. Needs a real per-file audit before estimating.
- Whether `mesh_off` in `td5_track_parser.c:267-269` (blob-relative fallback branch) ever fires on
  real assets.
- Whether any MODELS.DAT ships a nonzero on-disk `vertex_data_ptr` (the tri-state at
  `td5_render_mesh.c:1496-1503` implies yes; unverified against assets).
