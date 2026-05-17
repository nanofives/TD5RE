#!/usr/bin/env python3
"""
diff_replay_frames.py -- Per-sub-tick state diff for the snapshot-replay
harness (see td5_trace_replay.c and tools/frida_state_snapshot.js).

Both inputs use the 'TD5R' format:

  File header (32 bytes):
    +0x00  u32  magic         = 0x52354454 ('TD5R')
    +0x04  u16  version       = 1
    +0x06  u16  actor_count   = 6
    +0x08  u16  actor_stride  = 0x388
    +0x0A  u16  rs_count      = 6
    +0x0C  u16  rs_stride     = 0x11C
    +0x0E  u16  globals_bytes = 128
    +0x10  u32  record_count  = 0 (left at 0; readers consume to EOF)
    +0x14  u8[12] reserved

  Record (7272 bytes):
    +0x0000 u32 frame
    +0x0004 u32 sub_tick
    +0x0008 u32 sim_tick
    +0x000C u32 flags
    +0x0010 u8  actors[6][0x388]   (5424)
    +0x1540 u8  rs_table[6][0x11C] (1704)
    +0x1BE8 u8  globals[128]
    = 7272

Output: per-sub-tick labeled-field divergence count. The point of the
replay harness is that each sub_tick's diff isolates that single function
step's transform error (because state at the start of the sub-tick was
identical, assuming inject was active in [start_frame, end_frame)).

Subcommands:
  default       Per-sub-tick labeled count summary.
  --first       First diverging sub-tick + first field detail.
  --sub N       Full field-level dump at one sub_tick.
  --range LO:HI Field-level dumps across [LO, HI).
  --rs-only     Restrict diff to RS table region (ignore actors+globals).
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

try:
    from td5_actor_offsets import ACTOR_OFFSETS, ACTOR_STRIDE
except Exception as exc:
    print(f"WARN: td5_actor_offsets import failed ({exc}); using bare offsets.",
          file=sys.stderr)
    ACTOR_OFFSETS = {}
    ACTOR_STRIDE = 0x388

# ---- Format constants (must match writer) ------------------------------
FILE_MAGIC          = 0x52354454      # 'TD5R'
FILE_HEADER_BYTES   = 32
RECORD_HEADER_BYTES = 16
ACTOR_COUNT         = 6
RS_COUNT            = 6
RS_STRIDE           = 0x11C
GLOBALS_BYTES       = 128
RECORD_BYTES        = (RECORD_HEADER_BYTES
                       + ACTOR_COUNT * ACTOR_STRIDE   # 5424
                       + RS_COUNT * RS_STRIDE         # 1704
                       + GLOBALS_BYTES)               #  128
assert RECORD_BYTES == 7272, RECORD_BYTES

ACTORS_OFF  = RECORD_HEADER_BYTES
RS_OFF      = ACTORS_OFF + ACTOR_COUNT * ACTOR_STRIDE
GLOBALS_OFF = RS_OFF + RS_COUNT * RS_STRIDE

# ============================================================================
# Bucket-tagged divergence filter
# ============================================================================
# Every field that diverges between port and orig classifies into exactly one
# of four buckets:
#
#   process-local      Pointer / RNG / time-derived state that the port cannot
#                      match by construction (different heap, different PRNG,
#                      different clock). Hidden by default.
#
#   fpu-residue        ±1 LSB / ±256 fp8 differences from x87 80-bit → SSE2
#                      64-bit precision collapse + FISTP rounding edge cases.
#                      Bounded noise, doesn't affect gameplay. Hidden by default.
#
#   impl-divergent     ARCH-DIVERGENCEs documented in reference_arch_*.md —
#                      port deliberately uses a different addressing/encoding
#                      that's semantically equivalent but byte-different.
#                      Hidden by default.
#
#   (no bucket)        Real bugs. Field-level memory files (todo_*.md) own
#                      the follow-up. SHOWN by default — this is the "what to
#                      drive to 0" metric.
#
# The 2026-05-17 refactor split these out of the flat EXPECTED_DIVERGENT_*
# sets so that the default sweep output reflects ONLY the actionable work
# (investigated-deferred). Use --unfiltered or per-bucket --show-* flags to
# expose hidden buckets when investigating residue.
#
# Fields previously masked as "investigated-deferred" — steering_command,
# encounter_steering_cmd, engine_speed_accum, throttle_state, RS_TRACK_OFFSET_
# BIAS, RS[0x16], RS[0x17], RS[0x3D], RS[0x45] — are NO LONGER in this filter.
# They are the cascade chain tracked in todo_cascade_unwind_2026-05-17.md and
# now count toward the real-bug metric until the cascade fix lands.

BUCKET_PROCESS_LOCAL  = "process-local"
BUCKET_FPU_RESIDUE    = "fpu-residue"
BUCKET_IMPL_DIVERGENT = "impl-divergent"

# Default: hide all 3 irreducible buckets. CLI flags below can expose them.
DEFAULT_HIDDEN_BUCKETS = frozenset({
    BUCKET_PROCESS_LOCAL,
    BUCKET_FPU_RESIDUE,
    BUCKET_IMPL_DIVERGENT,
})


def _expand_range(start, end, step=1):
    return list(range(start, end, step))


# Actor offsets → bucket. Cite source per cluster.
ACTOR_FIELD_BUCKETS: dict[int, str] = {}

# --- process-local ----------------------------------------------------------
# Pointer fields (heap addresses) + frame counter.
for off in (0x1B0, 0x1B4, 0x1B8, 0x1BC, 0x338):
    ACTOR_FIELD_BUCKETS[off] = BUCKET_PROCESS_LOCAL

# --- fpu-residue ------------------------------------------------------------
# rotation_matrix[8] LSB (orig 80-bit → port 64-bit precision collapse)
ACTOR_FIELD_BUCKETS[0x140] = BUCKET_FPU_RESIDUE

# Wheel probes (body-relative, 4 wheels × 3 coords) — TransformShortVec3-
# ByRenderMatrixRounded @ 0x0042EB10 ±256 fp8 residue. See T3 audit:
# todo_chassis_snap_fix_2026-05-16.md.
for off in (0x090, 0x094, 0x098, 0x09C, 0x0A0, 0x0A4, 0x0A8, 0x0AC,
            0x0B0, 0x0B4, 0x0B8, 0x0BC,
            # Wheel contact (world-space, 4 wheels × 3 coords)
            0x0F0, 0x0F4, 0x0F8, 0x0FC, 0x100, 0x104, 0x108, 0x10C,
            0x110, 0x114, 0x118, 0x11C,
            # Wheel contact deltas (Agent I, 2026-05-16) — 4 wheels × 4 bytes
            0x270, 0x272, 0x274, 0x276,
            0x278, 0x27A, 0x27C, 0x27E,
            0x280, 0x282, 0x284, 0x286,
            0x288, 0x28A, 0x28C, 0x28E,
            # Wheel world hires (4 wheels × 3 coords, second-transform residue)
            0x298, 0x29C, 0x2A0, 0x2A4, 0x2A8, 0x2AC, 0x2B0, 0x2B4,
            0x2B8, 0x2BC, 0x2C0, 0x2C4):
    ACTOR_FIELD_BUCKETS[off] = BUCKET_FPU_RESIDUE

# body_probe span_accumulated / span_high_water — downstream of wheel-probe
# FPU residue per todo_body_probe_step_is_fpu_residue_downstream_2026-05-16.md.
for off in (0x004, 0x006, 0x014, 0x016, 0x024, 0x026, 0x034, 0x036):
    ACTOR_FIELD_BUCKETS[off] = BUCKET_FPU_RESIDUE

# --- impl-divergent: none currently in actor region -------------------------


# RS-table offsets → bucket.
RS_FIELD_BUCKETS: dict[int, str] = {
    # process-local: pointer into LEFT.TRK/RIGHT.TRK heap. Different process,
    # different addresses.
    0x000: BUCKET_PROCESS_LOCAL,  # RS_ROUTE_TABLE_PTR

    # impl-divergent: orig stores absolute pointer to .rdata script; port
    # stores absolute pointer to its own static arrays. Semantically same data.
    0x0E8: BUCKET_IMPL_DIVERGENT,  # RS_SCRIPT_BASE_PTR

    # impl-divergent: orig stores absolute pointer to current opcode word
    # (advanced +4/step); port stores integer index (advanced +1/step).
    # Trace-replay inject preserves port's form per commit 2a24dfa.
    0x0EC: BUCKET_IMPL_DIVERGENT,  # RS_SCRIPT_IP
}


# Globals offsets → bucket.
GLOBALS_FIELD_BUCKETS: dict[int, str] = {
    # process-local: per-instance race-end timer + per-frame timing fields
    0x08: BUCKET_PROCESS_LOCAL,  # g_raceEndFadeState
    0x0C: BUCKET_PROCESS_LOCAL,  # g_simTimeAccumulator
    0x10: BUCKET_PROCESS_LOCAL,  # sim_tick_budget
    0x18: BUCKET_PROCESS_LOCAL,  # normalized_frame_dt
    0x1C: BUCKET_PROCESS_LOCAL,  # instant_fps

    # impl-divergent: port init=0 vs orig=2 (frontend.c hardcoded difference)
    0x24: BUCKET_IMPL_DIVERGENT,  # g_splitscreenCount
}
# RNG-state + state-table fields the port doesn't currently pack into globals:
for off in (0x28, 0x2C, 0x30):
    GLOBALS_FIELD_BUCKETS[off] = BUCKET_PROCESS_LOCAL
for off in _expand_range(0x34, 0x60):  # player_control_bits, race_slot_*, etc.
    GLOBALS_FIELD_BUCKETS[off] = BUCKET_PROCESS_LOCAL


def field_bucket(region: str, f_off: int) -> str | None:
    """Return the bucket name for a field, or None if it's a real bug."""
    if region.startswith("actor"):
        return ACTOR_FIELD_BUCKETS.get(f_off)
    if region.startswith("rs"):
        return RS_FIELD_BUCKETS.get(f_off)
    if region == "globals":
        return GLOBALS_FIELD_BUCKETS.get(f_off)
    return None

# --- Globals field map -----------------------------------------------------
GLOBALS_FIELDS = [
    (0x00, 4, "u32", "g_gameState"),
    (0x04, 4, "u32", "g_gamePaused"),
    (0x08, 4, "u32", "g_raceEndFadeState"),
    (0x0C, 4, "u32", "g_simTimeAccumulator"),
    (0x10, 4, "f32", "g_simTickBudget"),
    (0x14, 4, "i32", "g_simulationTickCounter"),
    (0x18, 4, "f32", "g_normalizedFrameDt"),
    (0x1C, 4, "f32", "g_instantFPS"),
    (0x20, 4, "u32", "g_viewCount"),
    (0x24, 4, "u32", "g_splitscreenCount"),
    (0x28, 4, "u32", "g_randomSeedForRace"),
    (0x2C, 4, "u32", "g_raceSessionRandomSeed"),
    (0x30, 4, "u32", "g_raceRandomSeedTable[0]"),
    (0x34, 8, "bytes", "g_playerControlBits"),
    (0x3C, 24, "bytes", "g_raceSlotStateTable"),
    (0x54, 6, "bytes", "g_raceSlotPlayerFlags"),
    (0x5A, 6, "bytes", "g_raceOrderArray"),
]

# --- RouteState field map (RS[N] = dword index N within a 0x11C-byte slot) -
RS_FIELDS = [
    (0x00, 4, "u32", "RS[0x00] route_table_ptr"),
    (0x04, 4, "u32", "RS[0x01] route_data"),
    (0x08, 4, "i32", "RS[0x02] route_idx"),
    (0x0C, 4, "i32", "RS[0x03] route_table_selector"),
    (0x10, 4, "i32", "RS[0x04] route_target_offset"),
    (0x14, 4, "i32", "RS[0x05]"),
    (0x18, 4, "i32", "RS[0x06]"),
    (0x1C, 4, "i32", "RS[0x07]"),
    (0x20, 4, "i32", "RS[0x08]"),
    (0x24, 4, "i32", "RS[0x09] track_offset_bias"),
    (0x28, 4, "i32", "RS[0x0A]"),
    (0x2C, 4, "i32", "RS[0x0B]"),
    (0x30, 4, "i32", "RS[0x0C]"),
    (0x34, 4, "i32", "RS[0x0D]"),
    (0x38, 4, "i32", "RS[0x0E] left_deviation"),
    (0x3C, 4, "i32", "RS[0x0F] right_deviation"),
    (0x40, 4, "i32", "RS[0x10]"),
    (0x44, 4, "i32", "RS[0x11]"),
    (0x48, 4, "i32", "RS[0x12]"),
    (0x4C, 4, "i32", "RS[0x13]"),
    (0x50, 4, "i32", "RS[0x14] active_upper_bound"),
    (0x54, 4, "i32", "RS[0x15] active_lower_bound"),
    (0x58, 4, "i32", "RS[0x16]"),
    (0x5C, 4, "i32", "RS[0x17]"),
    (0x60, 4, "i32", "RS[0x18] fwd_track_component"),
    (0x64, 4, "i32", "RS[0x19] track_progress"),
]
# Synthesize rest of RS as unmapped dwords so diff still shows them.
def _rs_lookup(off):
    for f_off, f_size, f_dtype, f_name in RS_FIELDS:
        if f_off <= off < f_off + f_size:
            return (f_off, f_name, f_size, f_dtype)
    # Fall back to dword-grouping
    dw = (off // 4) * 4
    return (dw, f"RS[0x{dw // 4:02X}] (unmapped)", 4, "i32")


# ---- Record container -------------------------------------------------------
@dataclass
class Record:
    frame: int
    sub_tick: int
    sim_tick: int
    flags: int
    actors_bytes: bytes
    rs_bytes: bytes
    globals_bytes: bytes


def load_file(path: Path) -> list[Record]:
    data = path.read_bytes()
    if len(data) < FILE_HEADER_BYTES:
        raise ValueError(f"{path}: too small ({len(data)} bytes)")

    magic, version, ac, astride, rsc, rsstr, glb = struct.unpack_from(
        "<IHHHHHH", data, 0
    )
    if magic != FILE_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X} "
                         f"(expected 0x{FILE_MAGIC:08X})")
    if ac != ACTOR_COUNT or astride != ACTOR_STRIDE:
        raise ValueError(f"{path}: actor shape {ac}x{astride:#x}")
    if rsc != RS_COUNT or rsstr != RS_STRIDE:
        raise ValueError(f"{path}: rs shape {rsc}x{rsstr:#x}")
    if glb != GLOBALS_BYTES:
        raise ValueError(f"{path}: globals_bytes {glb}")

    records: list[Record] = []
    offset = FILE_HEADER_BYTES
    body_size = len(data) - offset
    if body_size % RECORD_BYTES:
        print(f"WARN: {path}: trailing {body_size % RECORD_BYTES} bytes "
              f"(partial record); ignored.", file=sys.stderr)
    n = body_size // RECORD_BYTES
    for _ in range(n):
        frame, sub_tick, sim_tick, flags = struct.unpack_from(
            "<IIII", data, offset
        )
        a0 = offset + ACTORS_OFF
        r0 = offset + RS_OFF
        g0 = offset + GLOBALS_OFF
        records.append(Record(
            frame=frame, sub_tick=sub_tick, sim_tick=sim_tick, flags=flags,
            actors_bytes=data[a0:r0],
            rs_bytes=data[r0:g0],
            globals_bytes=data[g0:g0 + GLOBALS_BYTES],
        ))
        offset += RECORD_BYTES
    return records


# ---- Field decoding -----------------------------------------------------
def _decode(dtype: str, size: int, raw: bytes) -> str:
    try:
        if dtype == "u8":   return f"{raw[0]}"
        if dtype == "i8":   return f"{struct.unpack('<b', raw[:1])[0]}"
        if dtype == "u16":  return f"{struct.unpack('<H', raw[:2])[0]}"
        if dtype == "i16":  return f"{struct.unpack('<h', raw[:2])[0]}"
        if dtype == "u32":  return f"{struct.unpack('<I', raw[:4])[0]}"
        if dtype == "i32":  return f"{struct.unpack('<i', raw[:4])[0]}"
        if dtype == "f32":  return f"{struct.unpack('<f', raw[:4])[0]:.6g}"
    except Exception:
        pass
    return raw[:min(size, 16)].hex()


def _actor_lookup(off):
    for f_off, (name, size, dtype, _src) in ACTOR_OFFSETS.items():
        if f_off <= off < f_off + size:
            return (f_off, name, size, dtype)
    return None


def _globals_lookup(off):
    for f_off, f_size, f_dtype, f_name in GLOBALS_FIELDS:
        if f_off <= off < f_off + f_size:
            return (f_off, f_name, f_size, f_dtype)
    return None


# ---- Diff core ----------------------------------------------------------
@dataclass
class FieldDiff:
    region: str        # "actor[i]" / "rs[i]" / "globals"
    slot: int
    field_off: int
    field_name: str
    port_val: str
    orig_val: str
    size: int
    dtype: str


def find_field_divergences(pr: Record, orig: Record,
                           rs_only: bool = False,
                           hidden_buckets: frozenset = DEFAULT_HIDDEN_BUCKETS,
                           ) -> list[FieldDiff]:
    """Diff two records, hiding fields in any of `hidden_buckets`.

    Default behaviour hides all 3 irreducible buckets (process-local +
    fpu-residue + impl-divergent) so callers see only real bugs.
    Pass `hidden_buckets=frozenset()` for the unfiltered view.
    """
    out: list[FieldDiff] = []
    seen: set[tuple[str, int, int]] = set()

    def _is_hidden(region: str, f_off: int) -> bool:
        return field_bucket(region, f_off) in hidden_buckets

    if not rs_only:
        for slot in range(ACTOR_COUNT):
            p = pr.actors_bytes[slot * ACTOR_STRIDE:(slot + 1) * ACTOR_STRIDE]
            o = orig.actors_bytes[slot * ACTOR_STRIDE:(slot + 1) * ACTOR_STRIDE]
            if p == o:
                continue
            for i in range(ACTOR_STRIDE):
                if p[i] == o[i]:
                    continue
                fld = _actor_lookup(i)
                if fld is None:
                    f_off, f_name, f_size, f_dtype = i, "(unmapped)", 1, "u8"
                else:
                    f_off, f_name, f_size, f_dtype = fld
                if _is_hidden("actor", f_off):
                    continue
                key = ("actor", slot, f_off)
                if key in seen: continue
                seen.add(key)
                f_p = p[f_off:f_off + f_size]
                f_o = o[f_off:f_off + f_size]
                out.append(FieldDiff(
                    region=f"actor[{slot}]", slot=slot,
                    field_off=f_off, field_name=f_name,
                    port_val=_decode(f_dtype, f_size, f_p),
                    orig_val=_decode(f_dtype, f_size, f_o),
                    size=f_size, dtype=f_dtype,
                ))

    # RS table
    for slot in range(RS_COUNT):
        p = pr.rs_bytes[slot * RS_STRIDE:(slot + 1) * RS_STRIDE]
        o = orig.rs_bytes[slot * RS_STRIDE:(slot + 1) * RS_STRIDE]
        if p == o:
            continue
        for i in range(RS_STRIDE):
            if p[i] == o[i]:
                continue
            f_off, f_name, f_size, f_dtype = _rs_lookup(i)
            if _is_hidden("rs", f_off):
                continue
            key = ("rs", slot, f_off)
            if key in seen: continue
            seen.add(key)
            f_p = p[f_off:f_off + f_size]
            f_o = o[f_off:f_off + f_size]
            out.append(FieldDiff(
                region=f"rs[{slot}]", slot=slot,
                field_off=f_off, field_name=f_name,
                port_val=_decode(f_dtype, f_size, f_p),
                orig_val=_decode(f_dtype, f_size, f_o),
                size=f_size, dtype=f_dtype,
            ))

    if not rs_only and pr.globals_bytes != orig.globals_bytes:
        for i in range(GLOBALS_BYTES):
            if pr.globals_bytes[i] == orig.globals_bytes[i]:
                continue
            fld = _globals_lookup(i)
            if fld is None:
                f_off, f_name, f_size, f_dtype = i, "(unmapped)", 1, "u8"
            else:
                f_off, f_name, f_size, f_dtype = fld
            if _is_hidden("globals", f_off):
                continue
            key = ("globals", -1, f_off)
            if key in seen: continue
            seen.add(key)
            f_p = pr.globals_bytes[f_off:f_off + f_size]
            f_o = orig.globals_bytes[f_off:f_off + f_size]
            out.append(FieldDiff(
                region="globals", slot=-1,
                field_off=f_off, field_name=f_name,
                port_val=_decode(f_dtype, f_size, f_p),
                orig_val=_decode(f_dtype, f_size, f_o),
                size=f_size, dtype=f_dtype,
            ))
    return out


def align_by_sub_tick(port: list[Record], orig: list[Record]):
    by_orig = {r.sub_tick: r for r in orig}
    return [(p, by_orig[p.sub_tick]) for p in port if p.sub_tick in by_orig]


def fmt_diff(d: FieldDiff) -> str:
    name = f"+0x{d.field_off:03X} {d.field_name}"
    return f"  {d.region:>10}  {name:<40}  port={d.port_val:>14}  orig={d.orig_val:>14}"


# ---- Commands ------------------------------------------------------------
def cmd_summary(pairs, rs_only, hidden_buckets):
    print(f"Examined {len(pairs)} matched sub_ticks.")
    print(f"{'sub_tick':>8}  {'sim_tick':>8}  {'flags':>5}  {'labeled':>8}  first_diverging_field")
    print("-" * 80)
    total_zero = 0
    for pr, orig in pairs:
        divs = find_field_divergences(pr, orig, rs_only=rs_only,
                                      hidden_buckets=hidden_buckets)
        divs.sort(key=lambda d: (d.region, d.field_off))
        if not divs:
            total_zero += 1
            continue
        first = divs[0]
        first_label = f"{first.region} +0x{first.field_off:03X} {first.field_name}"
        flagbit = pr.flags & 0x3
        print(f"{pr.sub_tick:>8}  {pr.sim_tick:>8}  {flagbit:>5}  "
              f"{len(divs):>8}  {first_label}")
    print("-" * 80)
    print(f"Clean sub_ticks: {total_zero}/{len(pairs)}")
    return 0


def cmd_first(pairs, rs_only, hidden_buckets):
    for pr, orig in pairs:
        divs = find_field_divergences(pr, orig, rs_only=rs_only,
                                      hidden_buckets=hidden_buckets)
        if divs:
            divs.sort(key=lambda d: (d.region, d.field_off))
            d = divs[0]
            print(f"FIRST DIVERGENCE @ sub_tick={pr.sub_tick} (sim_tick={pr.sim_tick}):")
            print(fmt_diff(d))
            print()
            print(f"  ({len(divs)} other field(s) also diverge at this sub_tick)")
            return 0
    print("No divergences across matched sub_ticks.")
    return 0


def cmd_sub(pairs, sub_tick, rs_only, hidden_buckets):
    for pr, orig in pairs:
        if pr.sub_tick != sub_tick:
            continue
        divs = find_field_divergences(pr, orig, rs_only=rs_only,
                                      hidden_buckets=hidden_buckets)
        divs.sort(key=lambda d: (d.region, d.field_off))
        if not divs:
            print(f"sub_tick={sub_tick}: clean")
            return 0
        print(f"sub_tick={sub_tick}, sim_tick={pr.sim_tick}: "
              f"{len(divs)} divergent field(s)")
        for d in divs:
            print(fmt_diff(d))
        return 0
    print(f"sub_tick={sub_tick} not found")
    return 2


def cmd_range(pairs, lo, hi, rs_only, hidden_buckets):
    any_div = False
    for pr, orig in pairs:
        if pr.sub_tick < lo or pr.sub_tick >= hi:
            continue
        divs = find_field_divergences(pr, orig, rs_only=rs_only,
                                      hidden_buckets=hidden_buckets)
        if not divs:
            print(f"sub_tick={pr.sub_tick:>4}  (clean)")
            continue
        any_div = True
        divs.sort(key=lambda d: (d.region, d.field_off))
        print(f"sub_tick={pr.sub_tick:>4} sim_tick={pr.sim_tick:>4} "
              f"({len(divs)} field(s))")
        for d in divs:
            print(fmt_diff(d))
    if not any_div:
        print(f"All matched sub_ticks in [{lo},{hi}) are clean.")
    return 0


def _resolve_hidden_buckets(args) -> frozenset:
    """Translate CLI flags into the set of buckets to hide.

    Default (no flags): hide all 3 irreducible buckets. The visible diff count
    is then the "real bug" metric (investigated-deferred + unmapped).
    --unfiltered shows all 4 buckets.
    --show-* flags each expose one specific bucket.
    """
    if args.unfiltered:
        return frozenset()
    hidden = set(DEFAULT_HIDDEN_BUCKETS)
    if args.show_process_local:
        hidden.discard(BUCKET_PROCESS_LOCAL)
    if args.show_fpu_residue:
        hidden.discard(BUCKET_FPU_RESIDUE)
    if args.show_archdiv:
        hidden.discard(BUCKET_IMPL_DIVERGENT)
    return frozenset(hidden)


# ---- Main ----------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("port_bin", type=Path,
                    help="Port-side capture (log/port_state_snapshot.bin)")
    ap.add_argument("orig_bin", type=Path,
                    help="Original-side capture "
                         "(tools/frida_csv/state_snapshot_original.bin)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--first", action="store_true",
                   help="Report first diverging sub_tick + first field.")
    g.add_argument("--sub", type=int,
                   help="Full field dump at one sub_tick.")
    g.add_argument("--range", metavar="LO:HI",
                   help="Field dumps across sub_ticks in [LO, HI).")
    ap.add_argument("--rs-only", action="store_true",
                    help="Restrict diff to the RS table.")
    # Bucket-visibility flags. Default hides all 3 irreducible buckets so
    # the displayed count = real-bug count (investigated-deferred + unmapped).
    ap.add_argument("--unfiltered", action="store_true",
                    help="Show all fields including all 3 irreducible buckets "
                         "(process-local + fpu-residue + impl-divergent).")
    ap.add_argument("--show-process-local", action="store_true",
                    help="Also show process-local fields (pointers, RNG, frame "
                         "counters, per-instance timing).")
    ap.add_argument("--show-fpu-residue", action="store_true",
                    help="Also show fpu-residue fields (wheel probes, contact, "
                         "hires, body_probe step — ±1 LSB / ±256 fp8 drift).")
    ap.add_argument("--show-archdiv", action="store_true",
                    help="Also show impl-divergent fields (ARCH-DIVERGENCEs: "
                         "script_ip pointer-vs-index, splitscreenCount, etc.).")
    args = ap.parse_args(argv)

    port = load_file(args.port_bin)
    orig = load_file(args.orig_bin)
    print(f"Loaded {len(port)} port records, {len(orig)} orig records.")
    pairs = align_by_sub_tick(port, orig)
    print(f"Matched {len(pairs)} sub_ticks by sub_tick index.")
    if not pairs:
        print("No overlapping sub_ticks; cannot diff.")
        return 2

    hidden = _resolve_hidden_buckets(args)
    if args.first:
        return cmd_first(pairs, args.rs_only, hidden)
    if args.sub is not None:
        return cmd_sub(pairs, args.sub, args.rs_only, hidden)
    if args.range:
        lo_s, hi_s = args.range.split(":", 1)
        return cmd_range(pairs, int(lo_s), int(hi_s), args.rs_only, hidden)
    return cmd_summary(pairs, args.rs_only, hidden)


if __name__ == "__main__":
    raise SystemExit(main())
