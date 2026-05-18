#!/usr/bin/env python3
"""Finalize TD5_d3d.exe confidence map.

Inputs:
  re/analysis/ghidra_confidence_map_2026-05-18.intermediate.json  (Phase 1-3 output)
  log/ghidra_callers_all.json                                     (Phase 4 caller counts)
  log/ghidra_bookmarks.json                                       (Phase 4 bookmark categories)

Outputs:
  re/analysis/ghidra_confidence_map_2026-05-18.csv
  re/analysis/ghidra_confidence_map_2026-05-18.summary.json
"""
from __future__ import annotations
import csv
import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOG_DIR = ROOT / "log"
ANALYSIS_DIR = ROOT / "re" / "analysis"

# --- Load intermediate ------------------------------------------------------
intermediate_path = ANALYSIS_DIR / "ghidra_confidence_map_2026-05-18.intermediate.json"
records = json.loads(intermediate_path.read_text())

# --- Load callers -----------------------------------------------------------
callers_path = LOG_DIR / "ghidra_callers_all.json"
callers_data = json.loads(callers_path.read_text())
caller_map = {}     # addr -> int count
caller_names = {}   # addr -> list of caller function names
for it in callers_data["items"]:
    fn = it.get("function", {})
    addr = fn.get("entry_point", "").lower()
    result = it.get("result", {})
    items = result.get("items", []) if isinstance(result, dict) else []
    caller_map[addr] = len(items)
    caller_names[addr] = sorted({c.get("name", "?") for c in items})

# --- Load bookmarks ---------------------------------------------------------
bookmarks_path = LOG_DIR / "ghidra_bookmarks.json"
bm_data = json.loads(bookmarks_path.read_text())
# Map address -> [(category, comment), ...]
bm_at = {}
for bm in bm_data["items"]:
    addr = bm["address"].lower()
    bm_at.setdefault(addr, []).append((bm["category"], bm["comment"]))

# Category counts (project-meaningful only)
bm_categories = Counter()
for bm in bm_data["items"]:
    bm_categories[bm["category"]] += 1

# --- Enrich records ---------------------------------------------------------
for r in records:
    addr = r["address"]
    r["callers_count"] = caller_map.get(addr, 0)
    r["caller_names_sample"] = caller_names.get(addr, [])[:5]
    bm_list = bm_at.get(addr, [])
    r["bookmark_tags"] = [c for c, _ in bm_list]
    r["bookmark_comments"] = [c for _, c in bm_list]

# --- Refine classification using bookmarks ----------------------------------
# A function bookmarked HOOK_POINT, PATCH_SITE, KEY_GLOBAL, CODE_CAVE counts
# as having Ghidra-side documentation. For our rubric, that pushes level-3 ports
# of these toward "documented" (still level 3 if no citation). We're already
# treating every function as named, so this just flags Ghidra-documented.
for r in records:
    r["ghidra_documented"] = bool(r["bookmark_tags"])

# --- CSV output -------------------------------------------------------------
csv_path = ANALYSIS_DIR / "ghidra_confidence_map_2026-05-18.csv"
with csv_path.open("w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh)
    w.writerow([
        "address", "name", "body_size", "level",
        "port_citations", "files_cited",
        "callers_count", "bookmark_tags", "ghidra_documented",
        "is_crt_or_lib", "precision_strong", "precision_weak",
        "sample_context",
    ])
    for r in sorted(records, key=lambda x: x["address"]):
        w.writerow([
            r["address"],
            r["name"],
            r["body_size"],
            r["level"],
            r["total_citations"],
            ";".join(r["files_cited"]),
            r["callers_count"],
            ";".join(r["bookmark_tags"]),
            int(r["ghidra_documented"]),
            int(r["is_crt"]),
            r["precision_strong"],
            r["precision_weak"],
            r["sample_context"].replace("\n", " ")[:300],
        ])
print(f"Wrote {csv_path}")

# --- Summary stats ----------------------------------------------------------
summary = {
    "total_functions": len(records),
    "by_level": Counter(r["level"] for r in records),
    "auto_named": sum(1 for r in records if r["is_auto_named"]),
    "crt_or_lib": sum(1 for r in records if r["is_crt"]),
    "cited_in_port": sum(1 for r in records if r["total_citations"] > 0),
    "ghidra_documented": sum(1 for r in records if r["ghidra_documented"]),
    "bookmark_categories": dict(bm_categories),
}
# also tier breakdowns for game-only (non-CRT)
game = [r for r in records if not r["is_crt"]]
summary["game_only"] = {
    "total": len(game),
    "by_level": Counter(r["level"] for r in game),
    "cited_in_port": sum(1 for r in game if r["total_citations"] > 0),
}

summary_path = ANALYSIS_DIR / "ghidra_confidence_map_2026-05-18.summary.json"
summary_path.write_text(json.dumps(summary, indent=2, default=str))
print(f"Wrote {summary_path}")
print(json.dumps(summary, indent=2, default=str))

# --- Top-N leverage signals -------------------------------------------------
# High-leverage low-confidence: level <= 3 (i.e., not ported in port source)
# AND callers_count >= 3
leverage = [r for r in records if r["level"] <= 3 and r["callers_count"] >= 3 and not r["is_crt"]]
leverage.sort(key=lambda r: (-r["callers_count"], -r["body_size"]))
print(f"\n=== Top 20 high-leverage low-confidence (L<=3, callers>=3, non-CRT) ===")
print(f"Total such functions: {len(leverage)}")
for r in leverage[:20]:
    print(f"  L{r['level']} callers={r['callers_count']:3d} sz={r['body_size']:5d}  {r['address']}  {r['name']}")

# Top 20 named-not-ported (Level 3, no citation, no CRT)
orphans = [r for r in records if r["level"] == 3 and not r["is_crt"] and r["total_citations"] == 0]
orphans.sort(key=lambda r: (-r["callers_count"], -r["body_size"]))
print(f"\n=== Top 20 named-not-ported (L3, 0 citations, non-CRT, by callers desc) ===")
print(f"Total such functions: {len(orphans)}")
for r in orphans[:20]:
    print(f"  callers={r['callers_count']:3d} sz={r['body_size']:5d}  {r['address']}  {r['name']}")

# Bookmark category breakdown
print(f"\n=== Bookmark categories ===")
for cat, n in bm_categories.most_common():
    print(f"  {cat:30s} {n}")

# Functions with HOOK_POINT bookmark but no port citation
hooked_orphans = [r for r in records if "HOOK_POINT" in r["bookmark_tags"] and r["total_citations"] == 0]
print(f"\n=== HOOK_POINT functions without port citation ({len(hooked_orphans)}) ===")
for r in hooked_orphans[:20]:
    print(f"  L{r['level']} {r['address']}  {r['name']}")
