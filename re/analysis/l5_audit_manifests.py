#!/usr/bin/env python3
"""Emit per-file L5 audit work-lists for parallel agents.

For each L4 function, lists the Ghidra address, name, body size, port files
that already cite it, and any triage evidence (helps the agent locate the
port equivalent quickly). One CSV per primary target file.

Output: re/analysis/l5_audit_manifests/<file>.csv
"""
from __future__ import annotations
import csv, json
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[2]
INTERMEDIATE = ROOT / "re" / "analysis" / "ghidra_confidence_map_2026-05-18.intermediate.json"
TRIAGE = ROOT / "re" / "analysis" / "l3_triage_2026-05-21.csv"
OUT_DIR = ROOT / "re" / "analysis" / "l5_audit_manifests"


def main():
    data = json.load(open(INTERMEDIATE))
    triage_ev = {r["address"]: r["evidence"]
                 for r in csv.DictReader(open(TRIAGE, encoding="utf-8"))}

    l4 = [r for r in data if r["level"] == 4]
    by_file = defaultdict(list)
    for r in l4:
        files = r.get("files_cited", [])
        primary = next((f for f in files if f.endswith(".c")), files[0] if files else "unknown")
        by_file[primary].append(r)

    OUT_DIR.mkdir(exist_ok=True)
    for fname, funcs in by_file.items():
        funcs_sorted = sorted(funcs, key=lambda r: r["address"])
        out_path = OUT_DIR / f"{fname}.csv"
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f)
            w.writerow(["address", "name", "body_size", "files_cited",
                        "phase1_evidence", "strong", "weak"])
            for r in funcs_sorted:
                w.writerow([
                    f"0x{r['address'].upper()}",
                    r["name"],
                    r["body_size"],
                    ";".join(r.get("files_cited", [])),
                    triage_ev.get(r["address"], ""),
                    r.get("precision_strong", 0),
                    r.get("precision_weak", 0),
                ])
        print(f"  {fname:35s} {len(funcs):>4} funcs -> {out_path.name}")


if __name__ == "__main__":
    main()
