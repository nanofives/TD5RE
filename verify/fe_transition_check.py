"""Round VA item 1: judge a frontend transition walk frame by frame.

Run a dev td5re.exe with TD5RE_FE_TRANSITION_PROBE=1 (one 'FE-PROBE' line per
frontend frame in log/frontend.log: screen, inner state, anim t, the first visible
button's drawn x, the title's drawn y; the same text is stamped bottom-left on the
frame, so a TD5RE_FRAMEDUMP=<...%05d.png> TD5RE_FRAMEDUMP_EVERY=1 sequence labels
itself) and an --InputScript walking the menus, then:

    python verify/fe_transition_check.py log/frontend.log

Per screen visit (frames between two screen changes) it fails on:
  SNAP-BACK  the slide-out had started (the button left its rest x by > 100 px)
             and a later frame of the same visit draws it back at rest
             (the OLD menu reappears for a frame before the next screen);
  VANISH     the slide-in had reached its rest pose (within 40 px) and a later
             frame of the visit jumps it off-screen (x <= -600) without a
             slide-out (the NEW menu disappears for a frame).
Rest x = the drawn x of the visit's first settled frame (C1). Visits whose button
set changes (N) restart the judgement. Exit code 0 = pass, 1 = fail.
"""
import re, sys

PAT = re.compile(r'FE-PROBE F(\d+) P(\d+) S(-?\d+) ST(-?\d+) C(\d) T([\d.]+) K(-?\d+) N(\d+) B(-?\d+) X(-?[\d.]+) TY(-?[\d.]+)')


def frames(path):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = PAT.search(line)
        if m:
            f, p, s, st, c, t, k, n, b, x, ty = m.groups()
            out.append(dict(F=int(f), S=int(s), ST=int(st), C=int(c), T=float(t), N=int(n), B=int(b),
                            X=float(x), TY=float(ty)))
    return out


def visits(fr):
    cur = []
    for r in fr:
        if cur and (r['S'] != cur[-1]['S'] or r['N'] != cur[-1]['N']):
            yield cur
            cur = []
        cur.append(r)
    if cur:
        yield cur


def judge(v):
    bad = []
    v = [r for r in v if r['B'] >= 0]
    if not v:
        return bad
    rest = next((r['X'] for r in v if r['C'] == 1), None)
    if rest is None:
        return bad
    left = False      # slide-out started
    arrived = False   # slide-in reached rest
    for i, r in enumerate(v):
        if abs(r['X'] - rest) <= 40:
            arrived = True
        if arrived and r['C'] == 1 and abs(r['X'] - rest) > 100:
            left = True
        if left and abs(r['X'] - rest) < 2:
            bad.append(('SNAP-BACK', r))
        if arrived and not left and r['X'] <= -600 and i > 0 and v[i - 1]['X'] > -600:
            bad.append(('VANISH', r))
    return bad


def main():
    fr = frames(sys.argv[1])
    allbad = []
    nvis = 0
    for v in visits(fr):
        nvis += 1
        for kind, r in judge(v):
            allbad.append((kind, r))
    for kind, r in allbad:
        print(f"FAIL {kind}: F{r['F']} screen {r['S']} state {r['ST']} C{r['C']} t {r['T']:.3f} button x {r['X']:.1f} title y {r['TY']:.1f}")
    print(f"fe_transition_check: {len(fr)} frames, {nvis} visits, {len(allbad)} bad frames -> {'FAIL' if allbad else 'PASS'}")
    return 1 if allbad else 0


if __name__ == '__main__':
    sys.exit(main())
