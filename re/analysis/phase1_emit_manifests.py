#!/usr/bin/env python3
"""Phase 1: emit citation-manifest blocks for CITED-BUT-UNTAGGED L3 functions.

Each block is a comment listing the Ghidra address + name for every function
the file ports. Adding `0x<addr>` strings into port source promotes each
listed function L3 -> L4 in build_confidence_map.py (cited without precision
keywords -> level 4).

CRITICAL: must NOT use any "strong" keywords from build_confidence_map.py's
precision_keywords() list (byte-faithful, [CONFIRMED @, precise-port,
[ARCH-DIVERGENCE, matches orig, ghidra-verified, etc.) — those would falsely
promote to L5 without an actual audit.

Outputs:
  re/analysis/phase1_manifests/<file>.manifest.txt   one per target file
  re/analysis/phase1_manifest_assignment.csv         function -> file mapping
"""
from __future__ import annotations
import csv
import re
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[2]
TRIAGE_CSV = ROOT / "re" / "analysis" / "l3_triage_2026-05-21.csv"
OUT_DIR = ROOT / "re" / "analysis" / "phase1_manifests"
ASSIGNMENT_CSV = ROOT / "re" / "analysis" / "phase1_manifest_assignment.csv"


# ---------------------------------------------------------------------------
# Name -> port file assignment (semantic heuristic)
# ---------------------------------------------------------------------------
def assign_file(name: str, evidence_file: str) -> str:
    n = name.lower()

    # 1. Strong semantic prefixes
    if "camera" in n or "fly_in" in n or "trackside" in n:
        return "td5_camera.c"
    if any(k in n for k in ("controllerbinding", "controlbinding", "controllerbinding")):
        return "td5_frontend.c"  # controller binding pages are frontend
    if any(k in n for k in ("racehud", "hudlayout", "speedo", "minimap", "hud_")):
        return "td5_hud.c"
    if any(k in n for k in ("vehicleaudio", "audio_mix", "sound_")):
        return "td5_sound.c"
    if "particle" in n or "tiretrack" in n or "smoke" in n or "vfx" in n:
        return "td5_vfx.c"
    if "trafficvehicle" in n or "actorvelocity" in n or "wheeljitter" in n:
        return "td5_physics.c"
    if any(k in n for k in ("trackdata", "trackstatic", "trackedstream", "tracksegment",
                              "tracktexture")):
        return "td5_track.c"
    if any(k in n for k in ("tgaarchive", "tgasurface", "tga.*page", "level.*texture",
                              "trackedmarker", "loadtraffic", "loadrace")):
        return "td5_asset.c"
    if any(k in n for k in ("displaymode", "wndmode", "windowmode", "backbuffer",
                              "primaryfront", "secondaryfront", "frontend",
                              "session", "lobby", "chat", "browser",
                              "raceresults", "screenname", "highscore",
                              "menu", "options", "binding")):
        return "td5_frontend.c"
    if any(k in n for k in ("rendermatrix", "renderstate", "projected", "polygon",
                              "primitive", "transform", "submitprojected",
                              "vehicleactormodel", "transformvector",
                              "viewmatrix", "transformshort")):
        return "td5_render.c"
    if "actor" in n or "boundingvolume" in n or "collisionalignment" in n:
        return "td5_physics.c"
    if "fadeout" in n or "crossfade" in n or "fadetrans" in n:
        return "td5_render.c"  # crossfade is in td5_render.c
    if "benchmark" in n:
        return "td5_game.c"
    if any(k in n for k in ("inflate", "huffman")):
        return "td5_inflate.c"

    # 2. Fall back to evidence file (the heuristic match), unless it's
    # a header — convert to .c equivalent if so.
    if evidence_file.endswith(".h"):
        c_form = evidence_file[:-2] + ".c"
        return c_form
    if evidence_file.endswith(".c"):
        return evidence_file
    return "td5_game.c"  # last-resort default


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    rows = list(csv.DictReader(open(TRIAGE_CSV, encoding="utf-8")))
    cited = [r for r in rows if r["label"] == "CITED-BUT-UNTAGGED"]
    print(f"CITED-BUT-UNTAGGED entries: {len(cited)}")

    # Assign target file
    by_file = defaultdict(list)
    assignments = []
    for r in cited:
        ev_file = r["evidence"].split(":", 1)[0] if ":" in r["evidence"] else ""
        target = assign_file(r["name"], ev_file)
        by_file[target].append(r)
        assignments.append({
            "address": r["address"],
            "name": r["name"],
            "evidence_file": ev_file,
            "evidence_kind": r["evidence"],
            "assigned_file": target,
            "match_confidence": (
                "strong" if (":0x" in r["evidence"] or ":name" in r["evidence"])
                else "weak" if ":density(" in r["evidence"]
                else "medium"
            ),
        })

    print()
    print("Assignment summary (after semantic remapping):")
    for f in sorted(by_file, key=lambda k: -len(by_file[k])):
        print(f"  {f:35s}  {len(by_file[f]):>3}  funcs")

    # Write assignment CSV
    with open(ASSIGNMENT_CSV, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["address", "name", "evidence_file",
                                          "evidence_kind", "assigned_file",
                                          "match_confidence"])
        w.writeheader()
        for a in sorted(assignments, key=lambda x: (x["assigned_file"], x["address"])):
            w.writerow(a)
    print(f"Wrote -> {ASSIGNMENT_CSV}")

    # Emit per-file manifest blocks
    OUT_DIR.mkdir(exist_ok=True)
    for target, funcs in by_file.items():
        funcs_sorted = sorted(funcs, key=lambda r: r["address"])
        lines = []
        lines.append("/* ============================================================")
        lines.append(" * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh")
        lines.append(" *")
        lines.append(" * The following L3 Ghidra functions are ported (or folded) into")
        lines.append(" * this file but were missed by build_confidence_map.py's")
        lines.append(" * 2026-05-18 citation scan due to snake_case rename or")
        lines.append(" * multi-line comment wraps. Listed here so the next confidence-")
        lines.append(" * map run promotes them L3 -> L4 (cited without precision")
        lines.append(" * keywords). Per-function audits remain a separate Phase 4 task.")
        lines.append(" *")
        lines.append(" * Source: re/analysis/l3_triage_2026-05-21.csv +")
        lines.append(" *         re/analysis/phase1_manifest_assignment.csv")
        lines.append(" *")
        for r in funcs_sorted:
            addr_8 = r["address"].upper()
            tag = ""
            ev = next((a for a in assignments
                       if a["address"] == r["address"]), None)
            if ev and ev["match_confidence"] == "weak":
                tag = "  (density-match, verify in Phase 4)"
            lines.append(f" *   0x{addr_8}  {r['name']}{tag}")
        lines.append(" */")
        manifest_path = OUT_DIR / f"{target}.manifest.txt"
        manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"  -> {manifest_path}  ({len(funcs)} funcs)")


if __name__ == "__main__":
    main()
