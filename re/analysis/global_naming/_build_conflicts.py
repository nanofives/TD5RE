#!/usr/bin/env python3
"""Build conflict inventory from batch files. Each address with >=2 proposals is a conflict."""
import json
import os
import re
import sys
from collections import defaultdict

BATCH_DIR = os.path.dirname(os.path.abspath(__file__))

def parse_batch(path):
    """Return list of dicts with address, size, name, confidence, evidence, port_mirror, tier, batch."""
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()

    # Extract tier from frontmatter
    m = re.search(r'^tier:\s*(T\d)', text, re.MULTILINE)
    tier = m.group(1) if m else 'T?'

    fn = os.path.basename(path)
    m = re.match(r'batch_(\d+)_', fn)
    batch_num = m.group(1) if m else '??'

    # Match "## Proposals" with optional " (globals)" suffix
    # Accept everything until next H2 header
    m = re.search(r'^## Proposals(?:\s*\([^)]*\))?\s*\n(.*?)(?=^## )', text, re.MULTILINE | re.DOTALL)
    if not m:
        return []
    block = m.group(1)

    proposals = []
    for line in block.splitlines():
        line = line.strip()
        if not line.startswith('|'):
            continue
        if line.startswith('|---') or '| address |' in line.lower() or '|address|' in line.lower():
            continue
        parts = [p.strip() for p in line.split('|')]
        parts = [p for p in parts if p != '']
        if len(parts) < 5:
            continue
        addr_raw = parts[0]
        m = re.match(r'`?(0x[0-9a-fA-F]{6,8})`?', addr_raw)
        if not m:
            continue
        address = m.group(1).lower()
        size = parts[1]
        name_raw = parts[2]
        name = name_raw.replace('`', '').replace('**', '').strip()
        confidence_raw = parts[3].lower().strip()
        if 'high' in confidence_raw:
            confidence = 'high'
        elif 'med' in confidence_raw:
            confidence = 'medium'
        elif 'low' in confidence_raw or 'comment' in confidence_raw:
            confidence = 'low'
        else:
            confidence = 'unknown'
        evidence = parts[4]
        port_mirror = parts[5] if len(parts) > 5 else ''

        proposals.append({
            'tier': tier,
            'batch': batch_num,
            'address': address,
            'size': size,
            'name': name,
            'confidence': confidence,
            'evidence': evidence,
            'port_mirror': port_mirror,
        })
    return proposals

def main():
    all_proposals = []
    batch_files = sorted([f for f in os.listdir(BATCH_DIR) if re.match(r'batch_\d+_.*\.md$', f)])
    for bf in batch_files:
        path = os.path.join(BATCH_DIR, bf)
        props = parse_batch(path)
        all_proposals.extend(props)

    print(f"Total proposals: {len(all_proposals)}")

    # Group by address
    by_addr = defaultdict(list)
    for p in all_proposals:
        by_addr[p['address']].append(p)

    # Conflicts: addresses with >=2 proposals
    conflicts = {addr: props for addr, props in by_addr.items() if len(props) >= 2}
    print(f"Conflict addresses (>=2 proposals): {len(conflicts)}")

    # Sort each conflict's proposals by batch number (winner first)
    for addr in conflicts:
        conflicts[addr].sort(key=lambda p: int(p['batch']))

    # Write conflict inventory JSON
    out_path = os.path.join(BATCH_DIR, '_conflicts.json')
    # Convert to JSON-serializable form
    out = {addr: props for addr, props in sorted(conflicts.items())}
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2)
    print(f"Wrote {out_path}")

    # Print breakdown
    n_two_way = sum(1 for c in conflicts.values() if len(c) == 2)
    n_three_way = sum(1 for c in conflicts.values() if len(c) == 3)
    n_four_plus = sum(1 for c in conflicts.values() if len(c) >= 4)
    print(f"  2-way conflicts: {n_two_way}")
    print(f"  3-way conflicts: {n_three_way}")
    print(f"  4+-way conflicts: {n_four_plus}")

    # Sum total proposals in conflicts
    total_props_in_conflicts = sum(len(c) for c in conflicts.values())
    n_alternates = total_props_in_conflicts - len(conflicts)  # losers
    print(f"  Total losing alternates (would be rejected): {n_alternates}")

if __name__ == '__main__':
    main()
