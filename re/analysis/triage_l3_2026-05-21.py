#!/usr/bin/env python3
"""Phase 0 triage classifier for L3 (RE'd but not ported) functions.

Reads re/analysis/ghidra_confidence_map_2026-05-18.csv, filters to level=3,
and labels each row with one of:

  ARCH-COLLAPSED-FMV    folded into td5_fmv.c stub (EA TGQ replacement)
  ARCH-COLLAPSED-ZLIB   folded into -lz dependency
  ARCH-COLLAPSED-LIBC   folded into CRT (snprintf/fopen/...)
  ARCH-COLLAPSED-DDRAW  folded into ddraw_wrapper / D3D11 path
  CITED-BUT-UNTAGGED    port code exists; citation scan missed it
  DEAD-CODE             0 callers + no port reference; candidate to defer
  TODO-PORT             genuinely missing port code

Outputs: re/analysis/l3_triage_2026-05-21.csv
"""
from __future__ import annotations
import csv
import re
from pathlib import Path
from typing import Dict, List, Tuple

ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT / "td5mod" / "src" / "td5re"
INPUT = ROOT / "re" / "analysis" / "ghidra_confidence_map_2026-05-18.csv"
OUTPUT = ROOT / "re" / "analysis" / "l3_triage_2026-05-21.csv"

# ---------------------------------------------------------------------------
# Known ARCH-COLLAPSED ranges (from CLAUDE.md + binary inspection)
# ---------------------------------------------------------------------------
# 0x00447000-0x00447FFF: zlib inflate (port links -lz)
# 0x00448000-0x00449FFF: CRT bridges (sprintf_game, fopen_game, ...)
# 0x00451000-0x0045BFFF: EA TGQ FMV codec (port replaces with td5_fmv.c stub)

ZLIB_RANGE = (0x00447000, 0x00448000)
LIBC_RANGE = (0x00448000, 0x0044A000)
FMV_RANGE  = (0x00451000, 0x0045C000)

# Manual overrides — entries verified by hand 2026-05-21 against port source.
# Format: address -> (label, evidence)
MANUAL_OVERRIDES = {
    # Both CrossFade16BitSurfaces variants are ported into td5_render_crossfade_surfaces.
    "0040cdc0": ("CITED-BUT-UNTAGGED", "td5_render.c:3666 td5_render_crossfade_surfaces (manual)"),
    "0040d190": ("CITED-BUT-UNTAGGED", "td5_render.c:3666 td5_render_crossfade_surfaces (manual)"),
    # NoOpHookStub is a literal 0-byte function in the original — there is
    # nothing to port. Treat as DEAD-CODE rather than TODO-PORT.
    "00418450": ("DEAD-CODE", "0-byte no-op function in orig"),
}

DDRAW_NAME_RE = re.compile(
    r"^(DDraw|CreateDDraw|CreateDirectSound|ReleaseDirectSound|"
    r"RestoreLostSurfaces|InitStreamDirectSound)",
    re.IGNORECASE,
)

DEAD_CODE_NAME_RE = re.compile(r"^caseD_")


# ---------------------------------------------------------------------------
# CamelCase -> snake_case for fuzzy matching against port source
# ---------------------------------------------------------------------------
def camel_to_snake(name: str) -> str:
    s1 = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    s2 = re.sub(r"([a-z\d])([A-Z])", r"\1_\2", s1)
    return s2.lower()


def name_tokens(name: str) -> List[str]:
    """Extract significant tokens (length>=4) from a CamelCase name."""
    parts = re.split(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])|_", name)
    return [p.lower() for p in parts if p and len(p) >= 4]


def fuzzy_cited(name: str, addr_hex: str, src_lower_map: Dict[str, str]) -> str:
    """Check if any port source mentions this function. Return matching file or ''.

    Match levels (any one promotes to CITED-BUT-UNTAGGED):
      1. Address hex string appears (0x004129B0 / 0x4129b0 / 4129b0 in comment).
      2. Full CamelCase name appears verbatim (possibly inside a comment).
      3. Full snake_case translation appears as an identifier.
      4. 2+ consecutive significant tokens appear in snake form
         (e.g. "cursor_overlay" for DeactivateFrontendCursorOverlay).
      5. >=60% of the significant tokens (>=3 of them) co-occur within a
         400-char window — catches multi-line comments mentioning the Ghidra
         name with line-wrapping.
    """
    # Skip overly generic names that would false-match (e.g. "ReturnZero", "CountSetBits")
    if len(name) < 8:
        return ""
    snake = camel_to_snake(name)
    bare = addr_hex.lstrip("0").lower()
    bare6 = addr_hex[2:] if addr_hex.startswith("00") else addr_hex
    tokens = name_tokens(name)

    for fname, src_lower in src_lower_map.items():
        # 1. Address hex string in any form
        for hex_form in (f"0x{addr_hex}", f"0x{bare6}", f"0x{bare}"):
            if hex_form.lower() in src_lower:
                return f"{fname}:{hex_form}"
        # 2. CamelCase name verbatim
        if name.lower() in src_lower:
            return f"{fname}:name"
        # 3. Exact snake_case match
        if snake in src_lower:
            return f"{fname}:{snake}"
        # 4. 2+ consecutive significant tokens in snake form
        if len(tokens) >= 2:
            for i in range(len(tokens) - 1):
                pair = f"{tokens[i]}_{tokens[i+1]}"
                # Skip very generic 2-token pairs (e.g. "set_state", "draw_to")
                if len(pair) < 12:
                    continue
                if pair in src_lower:
                    return f"{fname}:{pair}"
        # 5. Token-density check (multi-line comment match).
        # Operate on the raw-lowercased half only (before \x01 separator), so
        # the stripped-whitespace view doesn't double-count.
        raw_half = src_lower.split("\x01", 1)[0]
        if len(tokens) >= 3:
            # Find each token's positions; bail if too few tokens present.
            # Use SUBSTRING match (no \b) so tokens fused into CamelCase blobs
            # in lowercased comments still register.
            token_positions = []
            for t in tokens:
                positions = [m.start() for m in re.finditer(re.escape(t), raw_half)]
                token_positions.append(positions)
            # Need at least 60% of tokens present somewhere
            present = sum(1 for p in token_positions if p)
            if present >= max(3, int(0.6 * len(tokens))):
                # Check if some window contains >=60% of the tokens.
                # Cheap heuristic: pick the rarest token's positions, then for
                # each candidate position check how many other tokens fall in
                # +/-200 chars (tightened from 400 to reduce false matches).
                rarest_idx = min(range(len(tokens)),
                                 key=lambda i: len(token_positions[i]) or 9999)
                rarest_positions = token_positions[rarest_idx]
                threshold = max(3, int(0.6 * len(tokens)))
                for cp in rarest_positions:
                    lo, hi = cp - 200, cp + 200
                    hits = 1  # the rarest token itself
                    for j, positions in enumerate(token_positions):
                        if j == rarest_idx or not positions:
                            continue
                        if any(lo <= p <= hi for p in positions):
                            hits += 1
                    if hits >= threshold:
                        return f"{fname}:density({hits}/{len(tokens)})"
    return ""


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
def classify(row: Dict, src_lower_map: Dict[str, str]) -> Tuple[str, str]:
    """Return (label, evidence)."""
    addr_str = row["address"]
    # Manual overrides take precedence over everything.
    if addr_str in MANUAL_OVERRIDES:
        return MANUAL_OVERRIDES[addr_str]
    addr = int(addr_str, 16)
    name = row["name"]
    cc = int(row["callers_count"]) if row["callers_count"] else 0

    # Address-range buckets (highest confidence)
    if ZLIB_RANGE[0] <= addr < ZLIB_RANGE[1]:
        return "ARCH-COLLAPSED-ZLIB", "addr in zlib range"
    if LIBC_RANGE[0] <= addr < LIBC_RANGE[1]:
        return "ARCH-COLLAPSED-LIBC", "addr in CRT range"
    if FMV_RANGE[0] <= addr < FMV_RANGE[1]:
        return "ARCH-COLLAPSED-FMV", "addr in FMV codec range"

    # Name-based ARCH-COLLAPSED (DDraw helpers outside FMV range)
    if DDRAW_NAME_RE.match(name):
        return "ARCH-COLLAPSED-DDRAW", "DDraw/DirectSound name pattern"

    # Ghidra orphan-case labels (switch-table artifacts, always dead)
    if DEAD_CODE_NAME_RE.match(name):
        return "DEAD-CODE", "Ghidra caseD_* orphan"

    # Fuzzy citation check
    evidence = fuzzy_cited(name, row["address"], src_lower_map)
    if evidence:
        return "CITED-BUT-UNTAGGED", evidence

    # Zero-caller orphans without port references
    if cc == 0:
        return "DEAD-CODE", "0 callers, no port reference"

    return "TODO-PORT", ""


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    rows = list(csv.DictReader(open(INPUT, encoding="utf-8")))
    l3 = [r for r in rows if r["level"] == "3"]
    print(f"Loaded {len(l3)} L3 functions")

    # Load port sources (lowercased for case-insensitive matching).
    # Each file maps to two views: the raw lowercased text, AND a
    # whitespace-stripped view so multi-line comment-wrapped Ghidra names
    # (e.g. "DeactivateFrontend\nCursorOverlay") still match.
    src_lower_map: Dict[str, str] = {}
    for f in sorted(SRC_DIR.glob("*.c")) + sorted(SRC_DIR.glob("*.h")):
        src = f.read_text(encoding="utf-8", errors="replace").lower()
        # Append a stripped-whitespace copy so verbatim CamelCase name lookups
        # find the name across line breaks. Separator '\x01' between halves
        # prevents accidental cross-region matches.
        stripped = re.sub(r"\s+", "", src)
        src_lower_map[f.name] = src + "\x01" + stripped
    print(f"Loaded {len(src_lower_map)} port source files")

    # Classify
    classified = []
    for r in l3:
        label, evidence = classify(r, src_lower_map)
        classified.append({
            "address": r["address"],
            "name": r["name"],
            "body_size": r["body_size"],
            "callers_count": r["callers_count"] or "0",
            "label": label,
            "evidence": evidence,
        })

    # Sort: TODO-PORT first (highest caller count), then CITED-BUT-UNTAGGED,
    # then ARCH-COLLAPSED, then DEAD-CODE
    label_order = {
        "TODO-PORT": 0,
        "CITED-BUT-UNTAGGED": 1,
        "ARCH-COLLAPSED-DDRAW": 2,
        "ARCH-COLLAPSED-LIBC": 3,
        "ARCH-COLLAPSED-ZLIB": 4,
        "ARCH-COLLAPSED-FMV": 5,
        "DEAD-CODE": 6,
        "ARCH-COLLAPSED-NOOP": 7,
    }
    classified.sort(key=lambda r: (
        label_order.get(r["label"], 99),
        -int(r["callers_count"]),
        -int(r["body_size"]),
    ))

    # Write output
    with open(OUTPUT, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["label", "address", "name", "body_size",
                                          "callers_count", "evidence"])
        w.writeheader()
        for r in classified:
            w.writerow({
                "label": r["label"],
                "address": r["address"],
                "name": r["name"],
                "body_size": r["body_size"],
                "callers_count": r["callers_count"],
                "evidence": r["evidence"],
            })

    # Summary
    from collections import Counter
    counts = Counter(r["label"] for r in classified)
    print()
    print("=== Triage summary ===")
    for label in sorted(counts, key=lambda k: label_order.get(k, 99)):
        n = counts[label]
        print(f"  {label:25s} {n:>3d}")
    print(f"  {'TOTAL':25s} {sum(counts.values()):>3d}")
    print()
    print(f"Wrote -> {OUTPUT}")


if __name__ == "__main__":
    main()
