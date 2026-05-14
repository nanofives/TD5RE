# Pilot Audit — 0x00434670 ComputeSignedTrackOffset + 0x00434800 SampleTrackTargetPoint

**Date:** 2026-05-14
**Pool slot:** TD5_pool15
**Worktree:** `.claude/worktrees/precise-spline-target` on branch `precise-spline-target`
**Master tag:** pool15_spline
**Port-side functions:**
- `td5_track_compute_signed_offset` @ `td5_track.c:3355` (port of 0x00434670)
- `td5_track_compute_spline_position` @ `td5_track.c:3601` (older parallel port of 0x00434670 — buggy)
- `td5_track_sample_target_point` @ `td5_track.c:3410` (port of 0x00434800)

## Functions (Ghidra)

### 0x00434670 — `ComputeSignedTrackOffset(span_index, segment_distance, route_lane)` → int

Decomp summary (verbatim from listing 0x00434670..0x00434738, 75 instructions, 200 bytes):

```
strip = g_trackStripRecords + span_index * 0x18      // SAR by stride, span record
right_vi  = (uint16_t)strip[6]                       // span+0x06 right_vertex_index
type      = (uint8_t) strip[0]                       // span+0x00 span_type
lane_byte = (uint8_t) strip[3]                       // span+0x03 low nibble = lane_count

v_start   = g_trackVertexPool + right_vi * 6         // stride 6 vertex (3 int16)
type_offset = *(int32_t*)(0x473c6c + type * 8)       // <-- table at 0x473c6c, stride 8
end_vi    = right_vi + (lane_byte & 0xf) + type_offset
v_end     = g_trackVertexPool + end_vi * 6

dX = (int16_t)v_end[0]  -  (int16_t)v_start[0]
dZ = (int16_t)v_end[4]  -  (int16_t)v_start[4]       // z is at byte+4 in 6-byte vertex
delta = segment_distance - route_lane

// per-axis: round-to-zero divide by 256 of (delta * d)
sX_raw = dX * delta
sX     = (sX_raw + ((sX_raw >> 31) & 0xFF)) >> 8     // CDQ; AND 0xFF; ADD; SAR 0x8
sZ_raw = dZ * delta
sZ     = (sZ_raw + ((sZ_raw >> 31) & 0xFF)) >> 8     // ...same
// then plain SAR 0x8 of THE PRE-CDQ value too? NO -- the listing applies
// the round-to-zero idiom ONLY to one of the two; the other uses plain SAR.
// Re-check: at 0x004346cb-d1 EDX&0xFF + EAX (round-to-zero on sZ).
// at 0x004346e7-ed same for sX. Both get round-to-zero.
// Then 0x004346ef SAR EAX, 0x8 (sX plain SAR) and 0x004346f2 SAR ESI, 0x8 (sZ plain SAR).
// Wait — EAX has just been written by ADD, so the SAR is on the ROUND-TO-ZERO result.
// So the sequence is: imul; cdq; and 0xff; add; SAR 8. == (v + ((v>>31)&0xFF)) >> 8.
// ✓ Both axes use round-to-zero divide by 256.

mag_sq = sX*sX + sZ*sZ                                // 32-bit IMUL, IMUL, ADD
mag    = (int) sqrt((double)mag_sq)                   // FILD/FSQRT/__ftol

if (segment_distance < route_lane)
    return -mag
else
    return  mag
```

Listing-confirmed details:
- `[EDX*0x8 + 0x473c6c]` at 0x004346a3 — reads 4 bytes 4 bytes past the SampleTrackTargetPoint base
- FILD reads as int32 then FSQRT; __ftol truncates toward zero on FPU CW = 0x027F (after pilot 0x00403720's FPU CW fix). Master has `RC=01` round-toward-negative; __ftol overrides per-call to truncate (chop).
- NEG only fires when `segment_distance < route_lane` (JGE = unsigned >=)

### 0x00434800 — `SampleTrackTargetPoint(span_index, route_byte, out_xyz, lateral_bias)` → void

Decomp summary (88 instructions, 251 bytes 0x00434800..0x004348FB):

```
strip = g_trackStripRecords + span_index * 0x18
left_vi   = (uint16_t)strip[4]                       // span+0x04 LEFT vertex (note: NOT right!)
type      = (uint8_t) strip[0]
lane_byte = (uint8_t) strip[3]
origin_x  = *(int32_t*)(strip + 0x0C)                // span+0x0C origin_x  (24.0 fp)
origin_z  = *(int32_t*)(strip + 0x14)                // span+0x14 origin_z

v_left    = g_trackVertexPool + left_vi * 6
type_offset = *(int32_t*)(0x473c68 + type * 8)       // <-- DIFFERENT base than 0x00434670!
right_vi  = left_vi + (lane_byte & 0xf) + type_offset
v_right   = g_trackVertexPool + right_vi * 6

// pull world-space short coords (each + origin)
lx = (int16_t)v_left[0]  + origin_x
lz = (int16_t)v_left[4]  + origin_z
rx = (int16_t)v_right[0] + origin_x
rz = (int16_t)v_right[4] + origin_z

// linear 2-vertex interpolation, output as 24.8 FP
out[0] = (rx - lx) * route_byte + (lx << 8)
out[2] = (rz - lz) * route_byte + (lz << 8)

// build short tangent vector for ConvertFloatVec4ToShortAngles
local_8[0] = (int16_t)(rx - lx)                      // truncate to int16
local_8[1] = 0
local_8[2] = (int16_t)(rz - lz)
ConvertFloatVec4ToShortAngles(local_8, local_8)       // see below — normalize to length 4096

// apply lateral bias along the (left->right) edge direction (≡ tangent)
out[0] += ((tan_x * lateral_bias + ((tan_x * lateral_bias) >> 31 & 0xFFF)) >> 12) << 8
out[2] += ((tan_z * lateral_bias + ((tan_z * lateral_bias) >> 31 & 0xFFF)) >> 12) << 8
```

`ConvertFloatVec4ToShortAngles(short *in, short *out)` @ 0x0042CDB0:
```
fx = (float)(int32_t)(int16_t)in[0]    // FILD of int32 promoted from int16
fy = (float)(int32_t)(int16_t)in[1]    // we pass y=0
fz = (float)(int32_t)(int16_t)in[2]
len_sq = fx*fx + fy*fy + fz*fz
scale  = 4096.0f / sqrtf(len_sq)        // FDIVR with [0x00467378] (= 4096.0f) on x87
out[0] = (int16_t) __ftol(fx * scale)
out[1] = (int16_t) __ftol(fy * scale)
out[2] = (int16_t) __ftol(fz * scale)
```

Listing-confirmed details:
- `[EDX*0x8 + 0x473c68]` at 0x00434836 — reads the FIRST column of the table
- `pbVar1[3] & 0xf` — low nibble only (no shift / high-nibble use)
- `(short)(rx-lx)` truncation to int16 BEFORE ConvertFloatVec4ToShortAngles
- Lateral offset uses `(v*0xFFF)>>12` round-to-zero (12-bit divide)
- `<< 8` re-promotes the offset to 24.8 FP before adding to out
- Output is in 24.8 FP (low byte zeroed by `<<8`)

## Confirmed divergences (Static, vs Listing)

### D1 — k_target_vertex_offsets table is WRONG for compute_signed_offset and compute_spline_position **(HIGH IMPACT — corruption)**

Port file `td5_track.c:1299` declares `k_target_vertex_offsets[12][2]` with values for `[type][0]`:
`[0, 0, 0, -1, -1, -2, 0, -1, -1, -2, 0, 0]`

These values are from **0x473c68** (the FIRST column read by SampleTrackTargetPoint).

But the listing for **0x00434670 ComputeSignedTrackOffset** (and 0x004345B0 ComputeTrackSpanProgress) read at **0x473c6c** which is the SECOND column of each stride-8 row.

Memory at 0x473c68 (verified from pool15 memory_read, multiple independent reads; one 60-byte read returned a malformed buffer with an inserted zero at byte 44 — confirmed via 48-byte and 64-byte and 16-byte and 4-byte reads which all agree):
```
addr        value
0x473c68    0
0x473c6c    0
0x473c70    0
0x473c74    0
0x473c78   -1
0x473c7c    0
0x473c80   -1
0x473c84    0
0x473c88   -2
0x473c8c    0
0x473c90    0
0x473c94   -1
0x473c98    0
0x473c9c   -1
0x473ca0    0
0x473ca4   -2
0x473ca8    0
0x473cac    0
0x473cb0    0
0x473cb4    0
0x473cb8    0
0x473cbc    0
0x473cc0    0
0x473cc4    0
```

Stride-8 means each "type row" is 8 bytes = 2 columns. Per the asm:
- Col 0 (base 0x473c68) = SampleTrackTargetPoint reads this
- Col 1 (base 0x473c6c) = ComputeSignedTrackOffset / ComputeTrackSpanProgress read this

Resolving by type:
- **SampleTrackTargetPoint** values for types 0..11: `[0, 0, -1, -1, -2, 0, 0, 0, 0, 0, 0, 0]`
- **ComputeSignedTrackOffset / ComputeTrackSpanProgress** values for types 0..11: `[0, 0, 0, 0, 0, -1, -1, -2, 0, 0, 0, 0]`

Logical 2D layout matching the data:
```c
static const int8_t k_per_span_type_vertex_offsets[12][2] = {
    /* type   { col0_samplepoint, col1_signedoffset } */
    /* type 0 */  {  0,  0 },
    /* type 1 */  {  0,  0 },
    /* type 2 */  { -1,  0 },
    /* type 3 */  { -1,  0 },
    /* type 4 */  { -2,  0 },
    /* type 5 */  {  0, -1 },
    /* type 6 */  {  0, -1 },
    /* type 7 */  {  0, -2 },
    /* type 8 */  {  0,  0 },
    /* type 9 */  {  0,  0 },
    /* type 10 */ {  0,  0 },
    /* type 11 */ {  0,  0 },
};
```

The current port's `k_target_vertex_offsets[12][0]` table has non-zero values at types 3,4,5,7,8,9 — but the listing says:
- types 2,3,4 are non-zero in col0 (SampleTrackTargetPoint)
- types 5,6,7 are non-zero in col1 (ComputeSignedTrackOffset / ComputeTrackSpanProgress)

The port table is row-shifted by 1 AND uses col0 values for BOTH consumers. So:
- SampleTrackTargetPoint port uses k_target_vertex_offsets[][0] = [0, 0, 0, -1, -1, -2, 0, -1, -1, -2, 0, 0]
  - Correct values for col0: [0, 0, -1, -1, -2, 0, 0, 0, 0, 0, 0, 0]
  - Port: types 2/3/4/5/6/7/8/9 are all WRONG by +1 row.
- ComputeSignedTrackOffset/Progress port uses k_target_vertex_offsets[][0] = same wrong list.
  - Correct values for col1: [0, 0, 0, 0, 0, -1, -1, -2, 0, 0, 0, 0]
  - Port: types 3/4/5/6/7/8/9 wrong by row-shift + wrong-column.

Off-by-one (or off-by-two) and column-confusion means port computes the wrong `end_vi`, picking a vertex 1-2 rows off — producing wrong dX/dZ, wrong magnitude, wrong sign of result on junction-adjacent spans.

Also `td5_track_compute_spline_position` (the older parallel port at line 3601) skips the table entirely and uses `span_height_offset(sp)` (high nibble of byte 0x03), which is from a completely different decode and has **no equivalence** to the listing.

### D2 — `td5_track_compute_spline_position` is a stale duplicate port of 0x00434670 with multiple divergences

The port has two functions for the same listing function 0x00434670:
1. `td5_track_compute_signed_offset` (line 3355) — newer, closer to listing
2. `td5_track_compute_spline_position` (line 3601) — older

Caller `td5_ai.c:2004` uses the OLDER buggy version (`compute_spline_position`):
- Reads `vertex_idx_a` directly without `(uint16_t)` cast — the listing `XOR EAX,EAX; MOV AX,word ptr [ECX+0x6]` zero-extends to uint32, equivalent to `(uint32_t)(uint16_t)`. Port reads `(int)sp->right_vertex_index` which depends on field type — if `right_vertex_index` is uint16_t this is OK, if int16_t this sign-extends. Verify.
- Adds `span_height_offset(sp)` (high nibble of byte 0x03) instead of `k_target_vertex_offsets[][...]`. The high nibble is NOT used by 0x00434670 — only the low nibble (`& 0xf`) is read as `lane_count`. This is the wrong addend entirely.
- `if (lane_count < 1) lane_count = 1` — listing has no such clamp.
- `interp_x = (interp_x + ((interp_x >> 31) & 0xFF)) >> 8` ✓ matches listing's round-to-zero idiom.
- Uses `td5_isqrt` (integer sqrt) instead of `sqrt((double)...)`. Listing uses FILD/FSQRT/__ftol on a 32-bit int → x87 double precision → truncate. `td5_isqrt` is bit-different.

### D3 — `td5_track_compute_signed_offset` table-driven divergence + parameter name mismatch **(MEDIUM-HIGH)**

`td5_track_compute_signed_offset` (line 3355) currently:
- Reads `type_offset = k_target_vertex_offsets[type][0]` — wrong table (see D1)
- `vstart` uses `sp->right_vertex_index` ✓
- Round-to-zero ✓
- `sqrt((double)...)` ✓
- Sign branch ✓

Fix needed: the `type_offset` lookup should resolve to 0 for all types (per the listing's actual 0x473c6c read = all-zero column). Either:
- Define a new `k_signed_offset_type_offsets[12]` that's all zeros, OR
- Inline `type_offset = 0` since the value is constant

The cleaner port is to call this what the listing does: read at base 0x473c6c via a stride-8 table. The table is `{0, _, 0, _, 0, _, ..., 0, _}` with the underscore slots being col[0] (= SampleTrackTargetPoint's table). Logically: a 2D table `int32 k_per_span_type[12][2]` where `[N][0]` = SampleTrackTargetPoint offset, `[N][1]` = ComputeSignedTrackOffset offset. Per memory, `[N][1]` is always 0.

### D4 — SampleTrackTargetPoint `tan_x` / `tan_z` rounding **(LOW)**

Port uses `(int16_t)((fex / mag) * 4096.0f)` which is a C float→int conversion (toward-zero by default). The listing uses x87 FSQRT + FDIVR + FMUL + __ftol pattern: each component is computed as `int16_t((int)__ftol(component * (4096.0f/sqrtf(len_sq))))`.

For typical values the truncations agree, but at `tan_x` very close to a half-integer the round may differ by 1 LSB. Verifiable via Frida.

### D5 — SampleTrackTargetPoint debug log spam **(no-impact)**

Port has `s_probe_count < 16` debug logger in the function body. Not in original. Remove for byte-faithful build; keep behind a `#if PILOT_VERBOSE` for trace work.

## Plan

### Fix 1 (HIGH) — Correct `k_target_vertex_offsets[12][2]` to reflect ACTUAL DAT_00473C68 layout

Replace the existing table with:
```c
/* [CONFIRMED @ 0x00473C68 via memory_read pool15, 2026-05-14]
 * Stride-8 table: each "row" has 2 int32 columns.
 *   Col 0 (base 0x473c68) = SampleTrackTargetPoint  (0x00434836)
 *   Col 1 (base 0x473c6c) = ComputeSignedTrackOffset (0x004346A3)
 *                         + ComputeTrackSpanProgress (0x004345E4)
 *
 * Decoded per-type:
 *   type:  0  1  2  3  4  5  6  7  8  9 10 11
 *   col0:  0  0 -1 -1 -2  0  0  0  0  0  0  0
 *   col1:  0  0  0  0  0 -1 -1 -2  0  0  0  0
 *
 * Prior port had col0 = {0,0,0,-1,-1,-2,0,-1,-1,-2,0,0} which was a 1-row
 * shift AND used col0 for both consumers. The col1 values were never
 * applied at all. */
static const int8_t k_target_vertex_offsets[12][2] = {
    /* type  0 */ {  0,  0 },
    /* type  1 */ {  0,  0 },
    /* type  2 */ { -1,  0 },
    /* type  3 */ { -1,  0 },
    /* type  4 */ { -2,  0 },
    /* type  5 */ {  0, -1 },
    /* type  6 */ {  0, -1 },
    /* type  7 */ {  0, -2 },
    /* type  8 */ {  0,  0 },
    /* type  9 */ {  0,  0 },
    /* type 10 */ {  0,  0 },
    /* type 11 */ {  0,  0 },
};
```

Patch:
- `td5_track_compute_signed_offset` (line 3371) → use `k_target_vertex_offsets[type][1]`
- `td5_track_compute_span_progress` (line 3307) → use `k_target_vertex_offsets[type][1]`
- `td5_track_sample_target_point` (line 3450) → use `k_target_vertex_offsets[type][0]`

### Fix 2 (HIGH) — Remove or fix `td5_track_compute_spline_position` stale duplicate

`td5_ai.c:2004` should call `td5_track_compute_signed_offset` instead (both port the same 0x00434670 listing).

Delete `td5_track_compute_spline_position` body and either:
- Replace the entire function with a forwarder: `return td5_track_compute_signed_offset(...)`, OR
- Update `td5_ai.c:2004` to call `td5_track_compute_signed_offset` directly and delete the stale function entirely

The argument names differ:
- `compute_signed_offset(span_index, progress, route_byte)`
- `compute_spline_position(span_index, segment_distance, route_lane)`
- Listing: `(span_index, EBP=param_2, [ESP+0x1c]=param_3)` — param_2 = first int after span_index, param_3 = third int. The decomp comment "current progress slot" vs "requested route slot" matches `(progress, route_byte)`.

Both call sites in `td5_ai.c` pass `(segment_distance, route_lane)` to `compute_spline_position` and `(progress, route_byte)` to `compute_signed_offset`. These are the same listing args; the port-side naming is inconsistent. Resolve by picking one set (use the newer `progress/route_byte` per the AI code's data flow).

### Fix 3 (LOW) — SampleTrackTargetPoint `tan_x/tan_z` byte-faithful rounding

Replace `(int16_t)((fex / mag) * 4096.0f)` with explicit `(int16_t)(int32_t)lrintf((fex / mag) * 4096.0f)` matching __ftol's toward-zero truncation. (Master has FPU CW set; `lrintf` uses the same CW mode.) Actually __ftol = truncate-toward-zero regardless of FPU CW. So `(int16_t)(int)(value)` in C with default rounding-toward-zero on float→int is the byte-faithful match. Confirm via Frida.

### Fix 4 (LOW) — Remove probe debug log

Wrap `s_probe_count` debug log behind `#if 0` or remove. Has no effect on output state but adds branches per call.

## Frida probe schema (BOTH addresses)

`tools/frida_pool15_spline.js`:

Capture entry+exit per call. Key columns:

| Column | Source | Notes |
|---|---|---|
| sim_tick | g_simTickCounter | global counter |
| slot | resolved-by-actor / caller-tag | which actor |
| which_fn | "0x434670" or "0x434800" | distinguish |
| call_idx | per-tick increment | for ordering |
| caller_tag | return-address lookup | for SampleTarget callers |
| in_span_index | arg0 | |
| in_param2 | arg1 (segment_distance or route_byte) | |
| in_param3 | arg2 (route_lane or out_xyz ptr) | |
| in_lateral_bias | arg3 (sample only) | |
| in_origin_x / in_origin_z | from strip record | sanity |
| in_left_vi / in_right_vi | from strip | |
| in_lane_byte | strip[3] | |
| in_span_type | strip[0] | |
| in_vstart_x/z | from vertex pool | |
| in_vend_x/z | from vertex pool | |
| in_type_offset | DAT lookup | reveals which column |
| out_value | return EAX (0x434670) or out[0]/out[2] (0x434800) | |

Port emitter `td5_pilot_trace_pool15_spline.{c,h}` writes the same columns to `log/port/pool15_spline.csv` with `which_fn` distinguishing addresses.

## Fixes applied (this pilot — worktree precise-spline-target)

1. **Replaced** `k_target_vertex_offsets[12][2]` at `td5_track.c:1299-1312`
   with the listing-confirmed two-column layout:
   - col 0 (SampleTrackTargetPoint, base 0x473c68): {0,0,-1,-1,-2,0,0,0,0,0,0,0}
   - col 1 (ComputeSignedTrackOffset / ComputeTrackSpanProgress, base 0x473c6c): {0,0,0,0,0,-1,-1,-2,0,0,0,0}

2. **Updated** `td5_track_compute_span_progress` and
   `td5_track_compute_signed_offset` to read **col 1** instead of col 0,
   matching the listing's `[EDX*8 + 0x473c6c]` reads.
   `td5_track_sample_target_point` already used col 0, which matches the
   listing's `[EDX*8 + 0x473c68]` read.

3. **Replaced** `td5_track_compute_spline_position` body with a forwarder
   to `td5_track_compute_signed_offset` (both port the same listing
   function 0x00434670, the older variant had `span_height_offset`,
   `lane_count>=1` clamp, and `td5_isqrt` divergences).

4. **SampleTrackTargetPoint tan normalisation** — replaced `sqrtf` 32-bit
   path with a two-precision pattern matching ConvertFloatVec4ToShortAngles'
   x87 80-bit / 32-bit split (scale_hi for tan_x first __ftol; scale_lo
   for tan_z second __ftol). Approximates the listing's FPU stack
   behaviour at 0x42CE03 (FST) + 0x42CE10 (FLD reload).

5. **Removed** the verbose `s_probe_count` debug log inside
   `td5_track_sample_target_point` (was log spam, not in listing).

## Harness

- `tools/frida_pool15_spline.js` — paired probe, single CSV with
  `which_fn` column distinguishing 0x00434670 and 0x00434800.
- `td5mod/src/td5re/td5_pilot_trace_pool15_spline.{c,h}` — port-side
  emitter mirroring the Frida column schema.
- `td5_track.c` instrumented: emit at the return of
  `td5_track_compute_signed_offset` and `td5_track_sample_target_point`.
- Added to `TD5RE_SRCS` in `build_standalone.bat`.
- **Built successfully** — `td5re.exe` 1.74 MB.

## Reference

- Listing: 0x00434670..0x00434738 (75 ins) and 0x00434800..0x004348FB (88 ins) — Ghidra TD5_pool15, 2026-05-14
- Decompilation: same session
- Data tables (verified via mcp__ghidra__memory_read):
  - 0x00473C68 (SampleTrackTargetPoint): `[0, 0, -1, -1, -2, 0, -1, -1, -2, 0, 0, 0]`
  - 0x00473C6C (ComputeSignedTrackOffset / ComputeTrackSpanProgress): all zeros for types 0..11
- Callees: __ftol @ 0x0044817C, ConvertFloatVec4ToShortAngles @ 0x0042CDB0 (vector-normalize-to-4096)
- Callers of 0x00434670 (8): FindActorTrackOffsetPeer, RefreshActorTrackProgressOffset, InitializeActorTrackPose, UpdateSpecialTrafficEncounter, UpdateActorTrackBehavior, InitializeTrafficActorsFromQueue, UpdateActorTrackBounds, UpdateRaceActors
- Callers of 0x00434800 (2): UpdateActorTrackBehavior, ClassifyTrackOffsetClamp
- Port: `td5mod/src/td5re/td5_track.c` (lines 1299-1312, 3291-3345, 3355-3398, 3410-3536, 3601-3670)
