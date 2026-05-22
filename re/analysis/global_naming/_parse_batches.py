#!/usr/bin/env python3
"""Parse all 25 batch files into a single JSON list of proposals."""
import json
import os
import re
import sys

BATCH_DIR = os.path.dirname(os.path.abspath(__file__))

def parse_batch(path):
    """Return list of dicts with address, size, name, confidence, evidence, port_mirror, tier."""
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()

    # Extract tier from frontmatter
    m = re.search(r'^tier:\s*(T\d)', text, re.MULTILINE)
    tier = m.group(1) if m else 'T?'

    # Extract batch number from filename
    fn = os.path.basename(path)
    m = re.match(r'batch_(\d+)_', fn)
    batch_num = m.group(1) if m else '??'

    # Find ## Proposals section
    m = re.search(r'^## Proposals\s*\n(.*?)(?=^## )', text, re.MULTILINE | re.DOTALL)
    if not m:
        return []
    block = m.group(1)

    proposals = []
    for line in block.splitlines():
        line = line.strip()
        if not line.startswith('|'):
            continue
        # Skip header / separator
        if line.startswith('|---') or '| address |' in line.lower() or '|address|' in line.lower():
            continue
        # Split fields
        parts = [p.strip() for p in line.split('|')]
        # Leading/trailing empty parts from outer pipes
        parts = [p for p in parts if p != '']
        if len(parts) < 5:
            continue
        addr_raw = parts[0]
        # Match 0x followed by hex
        m = re.match(r'`?(0x[0-9a-fA-F]{6,8})`?', addr_raw)
        if not m:
            continue
        address = m.group(1).lower()
        size = parts[1]
        name_raw = parts[2]
        # Strip backticks
        name = name_raw.replace('`', '').strip()
        confidence_raw = parts[3].lower().strip()
        # normalize confidence
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
    summary = []
    for bf in batch_files:
        path = os.path.join(BATCH_DIR, bf)
        props = parse_batch(path)
        all_proposals.extend(props)
        high = sum(1 for p in props if p['confidence'] == 'high')
        med = sum(1 for p in props if p['confidence'] == 'medium')
        low = sum(1 for p in props if p['confidence'] == 'low')
        summary.append((bf, len(props), high, med, low))

    out_path = os.path.join(BATCH_DIR, '_all_proposals.json')
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(all_proposals, f, indent=2)

    print(f"Total proposals: {len(all_proposals)}")
    print(f"  high:   {sum(1 for p in all_proposals if p['confidence'] == 'high')}")
    print(f"  medium: {sum(1 for p in all_proposals if p['confidence'] == 'medium')}")
    print(f"  low:    {sum(1 for p in all_proposals if p['confidence'] == 'low')}")
    print(f"  unknown:{sum(1 for p in all_proposals if p['confidence'] == 'unknown')}")
    print()
    print("Per-batch breakdown:")
    for bf, t, h, m, l in summary:
        print(f"  {bf}: total={t} high={h} med={m} low={l}")

    print(f"\nWrote {out_path}")

if __name__ == '__main__':
    main()
