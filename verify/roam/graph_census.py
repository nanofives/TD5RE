#!/usr/bin/env python
"""GEO FREE ROAM G2: planner street graph vs drivable road surface census (the `roam-graph-census` dev test).

Runs `td5re.exe` with TD5RE_ROAM_GRAPH_CENSUS=1 (td5_roam_gcensus.c): every planner graph edge's centre line is
sampled every 2 m against the drivable surface of the casco ROAMTILE set, and every turn path the A* can build through
a node (arm end -> via points -> arm end) the same way. Writes log/roam_graph_census.{txt,csv}; this script copies them
next to --out (default _archive/roam/G2/) and prints the summary.

PASS = 0 edges with an off-road run > 5 m AND 0 interior nodes with a turn run > 15 m (nodes next to a tile with no file,
the casco border, are reported and not judged). Needs the casco tiles: `td5re.exe --RoamBuildTiles=casco` first.

Usage:  python -I verify/roam/graph_census.py [--exe td5re.exe] [--nofix] [--out DIR] [--tag NAME]
        --nofix  runs the pre-G2 graph (TD5RE_ROAM_GRAPH_NOFIX=1) for a before / after comparison
Exit 0 on PASS, 1 on FAIL, 2 when the census did not run.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'td5re.exe'))
    ap.add_argument('--nofix', action='store_true')
    ap.add_argument('--out', default=os.path.join(ROOT, '_archive', 'roam', 'G2'))
    ap.add_argument('--tag', default=None)
    ap.add_argument('--timeout', type=int, default=300)
    a = ap.parse_args()

    env = {k: v for k, v in os.environ.items() if not k.startswith('TD5RE_')}
    env['TD5RE_ROAM_GRAPH_CENSUS'] = '1'
    env['TD5RE_RT'] = '0'
    if a.nofix:
        env['TD5RE_ROAM_GRAPH_NOFIX'] = '1'
    txt = os.path.join(ROOT, 'log', 'roam_graph_census.txt')
    if os.path.exists(txt):
        os.remove(txt)
    try:
        subprocess.run([a.exe, '--Windowed=1'], cwd=ROOT, env=env, timeout=a.timeout,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        print('census timed out after %d s' % a.timeout)
        return 2
    if not os.path.exists(txt):
        print('no %s: the census did not run (casco tiles missing?)' % txt)
        return 2
    tag = a.tag or ('before' if a.nofix else 'after')
    os.makedirs(a.out, exist_ok=True)
    for ext in ('txt', 'csv'):
        src = os.path.join(ROOT, 'log', 'roam_graph_census.' + ext)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(a.out, 'census_%s.%s' % (tag, ext)))
    lines = open(txt, encoding='utf-8', errors='replace').read().splitlines()
    for ln in lines[:9]:
        print(ln)
    verdict = [ln for ln in lines if ln.startswith('result:')]
    print(verdict[-1] if verdict else 'no result line')
    return 0 if verdict and verdict[-1].startswith('result: PASS') else 1


if __name__ == '__main__':
    sys.exit(main())
