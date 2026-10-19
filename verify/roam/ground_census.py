#!/usr/bin/env python
"""GEO FREE ROAM G4: ground grid vs road surface census (grass flickering through the road).

Runs `td5re.exe` with TD5RE_ROAM_GROUND_CENSUS=1 (td5_roam_render.c): the ground of every casco ROAMTILE is built
exactly as the renderer bakes it (16 m lattice cells, HEIGHT.R16 through td5_roam_height, dropped
TD5RE_ROAM_RENDER_GROUND_DROP_CM, the G4 clamp unless --nofix), then every road primitive (lane quads as the two
triangles the renderer draws, junction patch triangles) is sampled every 50 cm. d = ground as drawn - road as drawn:
the ground is meant to sit `drop` below the road; d near 0 z-fights, d > 0 covers the road. Per ground cell under
the road the worst d is kept. Writes log/roam_ground_census.{txt,csv}; this script copies them to
--out/census_<tag>.{txt,csv} (default _archive/roam/G4/) and prints the summary.

PASS = 0 ground cells whose worst d is above -GAP cm (--gap, default 10).
Needs the casco tiles: `td5re.exe --RoamBuildTiles=casco` first (or --tiles DIR for another tile directory).

[G5] The census also walks every kerb boundary edge (ROAD EDGE block): `over` = how far the clamp put the ground
under min(unclamped ground, road edge - drop), i.e. the hang the clamp added beyond what it needs (steep roads).

Usage:  python -I verify/roam/ground_census.py [--exe td5re.exe] [--nofix | --g4] [--pave] [--out DIR] [--tag NAME] [--tiles DIR]
        --nofix  the pre-G4 ground (TD5RE_ROAM_GROUND_NOFIX=1) for a before / after comparison
        --g4     the G4 4-cell-minimum clamp (TD5RE_ROAM_GROUND_G4=1), the pre-G5 ground
        --pave   the G4b pavement census instead (TD5RE_ROAM_PAVE_CENSUS=1, log/roam_pavement_census.*, ~30 s);
                 exit 0 when it ran
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
    ap.add_argument('--g4', action='store_true')
    ap.add_argument('--pave', action='store_true')
    ap.add_argument('--out', default=os.path.join(ROOT, '_archive', 'roam', 'G4'))
    ap.add_argument('--tag', default=None)
    ap.add_argument('--tiles', default=None)
    ap.add_argument('--gap', type=float, default=10.0)
    ap.add_argument('--timeout', type=int, default=600)
    a = ap.parse_args()

    env = {k: v for k, v in os.environ.items() if not k.startswith('TD5RE_')}
    env['TD5RE_ROAM_GROUND_CENSUS'] = '1'
    env['TD5RE_RT'] = '0'
    if a.nofix:
        env['TD5RE_ROAM_GROUND_NOFIX'] = '1'
    if a.g4:
        env['TD5RE_ROAM_GROUND_G4'] = '1'
    if a.pave:
        env['TD5RE_ROAM_PAVE_CENSUS'] = '1'
    if a.tiles:
        env['TD5RE_ROAM_GROUND_CENSUS_DIR'] = os.path.abspath(a.tiles)
    base = 'roam_pavement_census' if a.pave else 'roam_ground_census'
    txt = os.path.join(ROOT, 'log', base + '.txt')
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
    tag = a.tag or ('before' if a.nofix else ('g4' if a.g4 else 'after'))
    os.makedirs(a.out, exist_ok=True)
    for ext in ('txt', 'csv'):
        src = os.path.join(ROOT, 'log', base + '.' + ext)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(a.out, '%s_%s.%s' % ('pave' if a.pave else 'census', tag, ext)))
    ed = os.path.join(ROOT, 'log', 'roam_ground_edge.csv')
    if not a.pave and os.path.exists(ed):
        shutil.copyfile(ed, os.path.join(a.out, 'edge_%s.csv' % tag))
    lines = open(txt, encoding='utf-8', errors='replace').read().splitlines()
    if a.pave:
        for ln in lines[:14]:
            print(ln)
        return 0
    for ln in lines[:20]:
        if ln.startswith('ROAD EDGE'):
            break
        print(ln)
    for i, ln in enumerate(lines):
        if ln.startswith('ROAD EDGE'):
            for l2 in lines[i:i + 14]:
                print(l2)
            break
    bad = None
    for ln in lines:
        p = ln.split()
        if len(p) == 3 and p[0].lstrip('+-').isdigit() and float(p[0]) == -a.gap:
            bad = int(p[2])
    if bad is None:
        print('threshold -%g cm not in the census table' % a.gap)
        return 2
    print('result: %s (%d ground cells with worst d above -%g cm)' % ('PASS' if bad == 0 else 'FAIL', bad, a.gap))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
