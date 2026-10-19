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

Usage:  python -I verify/roam/ground_census.py [--exe td5re.exe] [--nofix] [--out DIR] [--tag NAME] [--tiles DIR]
        --nofix  the pre-G4 ground (TD5RE_ROAM_GROUND_NOFIX=1) for a before / after comparison
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
    if a.tiles:
        env['TD5RE_ROAM_GROUND_CENSUS_DIR'] = os.path.abspath(a.tiles)
    txt = os.path.join(ROOT, 'log', 'roam_ground_census.txt')
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
        src = os.path.join(ROOT, 'log', 'roam_ground_census.' + ext)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(a.out, 'census_%s.%s' % (tag, ext)))
    lines = open(txt, encoding='utf-8', errors='replace').read().splitlines()
    for ln in lines[:20]:
        print(ln)
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
