#!/usr/bin/env python3
"""Convert wave1_f_comment_sync_plan.csv into ghidra-apply-compatible batch
markdown files.

Each batch file targets ~60 unique addresses. Comments are deduplicated per
address — multiple port-source headers referencing the same orig address get
consolidated into a single Ghidra PLATE comment.

Output: re/analysis/function_naming/batch_NN_wave2b_audit_<group>.md
"""
from __future__ import annotations
import csv
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INPUT = ROOT / "analysis" / "followup_sessions" / "wave1_f_comment_sync_plan.csv"
OUT_DIR = ROOT / "analysis" / "function_naming"
OUT_DIR.mkdir(exist_ok=True, parents=True)


def main():
    rows = list(csv.DictReader(open(INPUT, encoding="utf-8")))
    print(f"Total rows: {len(rows)}")

    # Group by address; drop rows with empty address
    by_addr: dict[str, list[dict]] = defaultdict(list)
    skipped = 0
    for r in rows:
        addr = (r.get("orig_address") or "").strip()
        if not addr:
            skipped += 1
            continue
        by_addr[addr].append(r)

    print(f"Unique addresses with comments: {len(by_addr)}")
    print(f"Skipped (no address): {skipped}")

    # Sort addresses; chunk into batches of 60
    addrs = sorted(by_addr.keys())
    BATCH_SIZE = 60
    batches = [addrs[i:i + BATCH_SIZE] for i in range(0, len(addrs), BATCH_SIZE)]

    for batch_idx, batch_addrs in enumerate(batches):
        batch_num = 50 + batch_idx  # avoid colliding with existing global_naming batches
        # Address-range tag for filename
        first = batch_addrs[0].lower().replace("0x", "")
        last = batch_addrs[-1].lower().replace("0x", "")
        tag = f"audit_{first}_{last}"
        out_path = OUT_DIR / f"batch_{batch_num:02d}_wave2b_{tag}.md"

        lines = []
        lines.append("---")
        lines.append(f"batch: {batch_num}")
        lines.append(f"area: wave2b_audit_comment_sync")
        lines.append("tier: T6")
        lines.append("target_todos: []")
        lines.append("ghidra_session: (per-batch, ghidra-apply opens master)")
        lines.append(f"analyzed_addresses: {','.join(batch_addrs)}")
        lines.append("agent: claude-opus-4-7")
        lines.append("date: 2026-05-22")
        lines.append("---")
        lines.append("")
        lines.append(f"# Wave 2B audit-comment sync — batch {batch_num} ({len(batch_addrs)} addresses)")
        lines.append("")
        lines.append("## Summary")
        lines.append("")
        lines.append(f"- Addresses in this batch: {len(batch_addrs)}")
        lines.append("- All proposals are confidence=low (comment-only, no rename)")
        lines.append("- Each address receives a consolidated PLATE comment derived from port-source audit headers")
        lines.append("")
        lines.append("## Methodology")
        lines.append("")
        lines.append("Pure derived from `re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv` —")
        lines.append("port [CONFIRMED @ ...] / [ARCH-DIVERGENCE: ...] audit-header references extracted")
        lines.append("and deduped per address. Comment text combines unique header summaries.")
        lines.append("")
        lines.append("## Proposals (functions)")
        lines.append("")
        lines.append("| address | current_name | proposed_name | confidence | evidence | port_mirror |")
        lines.append("|---|---|---|---|---|---|")

        for addr in batch_addrs:
            entries = by_addr[addr]
            # Build consolidated evidence: take unique short comments, join with " | "
            seen = set()
            parts = []
            for e in entries:
                t = (e.get("comment_text_short") or "").strip()
                # Strip pipe chars (would break markdown table)
                t = t.replace("|", "/")
                if t and t not in seen:
                    seen.add(t)
                    parts.append(t)
            evidence = " | ".join(parts[:3])  # cap at 3 unique summaries
            if len(evidence) > 400:
                evidence = evidence[:397] + "..."

            # port_mirror: use first row's file:line
            first_row = entries[0]
            port_mirror = f"{first_row.get('port_file','')}:{first_row.get('port_line','')}"

            # The skill needs current_name + proposed_name. Use placeholder.
            lines.append(f"| {addr} | _unchanged_ | _unchanged_ | low | {evidence} | {port_mirror} |")

        lines.append("")
        lines.append("## Key discoveries")
        lines.append("")
        lines.append("- (Mechanical comment-sync batch; no new findings.)")
        lines.append("")
        lines.append("## Out-of-scope finds")
        lines.append("")
        lines.append("- (None — this batch only consolidates existing port audit headers.)")
        lines.append("")
        lines.append("## TODO impact")
        lines.append("")
        lines.append("- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.")
        lines.append("")

        out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"  -> batch_{batch_num:02d} ({len(batch_addrs)} addresses) — {out_path.name}")

    print()
    print(f"Total batches emitted: {len(batches)}")


if __name__ == "__main__":
    main()
