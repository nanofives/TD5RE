#!/usr/bin/env python3
"""Why do two generated level dirs hash differently?  [ROUND 1028 R]

Short answer, measured 2026-10-13 at master 66a9f923: usually they DON'T, and
the two "different" hashes are the same bytes under two different algorithms.

THE DRIFT WAS A HASH MIX-UP.  Rounds 1025-1028 all carried the warning that
"a fresh worktree builds level091 to a DIFFERENT MODELS.DAT than main with the
same exe (seen 61410E76... and 3CD0EB1C... vs main be65458d...)".  Measured on
a round-1028 sibling worktree that still held that exact file:

    MODELS.DAT  12332744 B   md5 BE65458D6C43DF5B7162C4F0F51F4FAD
                             sha256 61410E76AC26759EC09C0DDFF7FCBB03...

ONE FILE.  `be65458d` is its md5 (what the round-1025 memory and every brief
since recorded); `61410E76` is the sha256 prefix that verify/geo_tags_identity
.ps1 prints.  Comparing the brief's md5 against the gate's sha256 "proved"
drift that was never there.  That is why this script prints BOTH.

THE WORKTREE IS NOT A VARIABLE either.  With main's td5re.exe copied in and
seed 2054677019, every build of level091 at 66a9f923 came out byte-identical
across three trees, two build paths and three different env= hashes:

    main tree,      gate run         env=c6f4a72f   12387408 B
    fresh worktree, gate run         env=c6f4a72f   12387408 B
    fresh worktree, gate run again   env=c6f4a72f   12387408 B
    fresh worktree, plain race       env=f0c5b31d   12387408 B   (STREAMED path)
    sibling worktree, geo_tour       env=a4bb7699   12387408 B   (STREAMED path)
        all -> sha256 B568E335...  md5 931E1473383EA069F395940A9B719236

STRIP.DAT and TEXTURES.DAT identical too.  Supporting measurements:

  * the whole 1.1 GB re/assets/geo cache -- 1413 non-tile files -- was
    md5-identical in both trees AND unchanged by the build (no cache mutation)
  * all 13442 re/assets files matched on name+size, with ONE exception:
    re/assets/frontend/lang/es_AR.txt, 22432 B (LF) in main vs 23084 B (CRLF)
    in a worktree -- core.autocrlf=true converts it on checkout and the file
    carries no eol attribute.  A frontend string table; it reaches no generator.

What DOES move the bytes is a knob that changes generation, and GENSTAMP.TXT
already records the knob set as env=.  Same tree, same exe, same seed, only
TD5RE_GEO_MASTARM=0 added:  12298300 B, sha256 C148BB65..., TEXTURES.DAT also
different, STRIP.DAT unchanged (scenery moved, the road did not).

    seed=  the roll (TD5RE_AUTOTRACK_SEED, else derived from the place slug)
    spec=  the spec struct + the geo route's own nodes
    env=   FNV1a over EVERY TD5RE_* variable in the environment
           (tg_env_hash, td5_trackgen.c:5175 -- the exclusion list covers only
           the control port, window title, RT/framedump/D3D12, free-cam and a
           few post-build knobs; every other TD5RE_* knob counts).  NOTE a
           different env= does NOT imply different bytes: the tour and race
           runs above differed in env= and still matched to the byte.
    exe=   mtime<<20 ^ size of the running exe

So before reporting drift: hash both sides with the SAME algorithm (this
script does), then read both stamps.

    python verify/geo_level_identity.py re/assets/levels/level091
    python verify/geo_level_identity.py A B    # attribute a difference; exit 1 if bytes differ
"""
import hashlib
import os
import sys

HASHED = ("MODELS.DAT", "STRIP.DAT", "TEXTURES.DAT")

WHY = {
    "seed": "a different roll (TD5RE_AUTOTRACK_SEED / a different place slug)",
    "spec": "a different spec or a different geo route",
    "env":  "a different TD5RE_* knob set -- diff the two launches (but see the"
            " header: env= alone often does NOT move the bytes)",
    "exe":  "a DIFFERENT BUILD (each child builds its own exe; copy one in to compare)",
}


def _digests(path):
    """(sha256_prefix32, md5) in one pass -- quoting only one of these is how
    the 2025-2028 'worktree drift' reports were manufactured."""
    s, m = hashlib.sha256(), hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            s.update(chunk)
            m.update(chunk)
    return s.hexdigest().upper()[:32], m.hexdigest().upper()


def _stamp(level):
    """GENSTAMP.TXT as a dict of its key=value tokens, or {} when absent."""
    path = os.path.join(level, "GENSTAMP.TXT")
    if not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="ascii", errors="replace") as f:
        text = f.read().strip()
    out = {"_raw": text}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def report(level):
    """Print hashes + build identity for one level dir. Returns its identity."""
    print("=== %s" % level)
    if not os.path.isdir(level):
        print("  level dir MISSING")
        return None
    digests = {}
    for name in HASHED:
        path = os.path.join(level, name)
        if os.path.isfile(path):
            sha, md5 = _digests(path)
            digests[name] = (sha, md5)
            print("  %-13s %10d bytes  sha256:%s" % (name, os.path.getsize(path), sha))
            print("  %-13s %10s         md5:%s" % ("", "", md5))
        else:
            print("  %-13s MISSING" % name)

    st = _stamp(level)
    if st:
        print("  GENSTAMP      %s" % st["_raw"])
    else:
        print("  GENSTAMP      MISSING -- this dir was not written by the generator")

    # Timing note only. A streamed build publishes MODELS.DAT from the scenery
    # worker tens of seconds after the geometry pass wrote STRIP.DAT; measured
    # byte-identical to the synchronous double build when allowed to FINISH.
    # The hazard is a run killed on a tick cap, which hashes a partial file.
    models = os.path.join(level, "MODELS.DAT")
    strip = os.path.join(level, "STRIP.DAT")
    if os.path.isfile(models) and os.path.isfile(strip):
        dt = os.path.getmtime(models) - os.path.getmtime(strip)
        if dt > 5.0:
            print("  build path    STREAMED (MODELS.DAT written %.0f s after STRIP.DAT by" % dt)
            print("                the scenery worker). Measured byte-identical to a")
            print("                synchronous build IF the worker finished -- a run killed")
            print("                early hashes a partial file. Check race.log for the")
            print("                'scenery stream: worker finished' line.")
        else:
            print("  build path    synchronous (MODELS.DAT within %.0f s of STRIP.DAT)" % dt)
    return {"stamp": st, "digests": digests}


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    results = [(lvl, report(lvl)) for lvl in argv]
    if len(results) < 2:
        return 0

    print("=== verdict")
    (la, ra), (lb, rb) = results[0], results[1]
    if not ra or not rb:
        print("  CANNOT COMPARE -- a level dir is missing")
        return 1

    same = all(ra["digests"].get(n) and ra["digests"].get(n) == rb["digests"].get(n)
               for n in HASHED)
    moved = [k for k in ("seed", "spec", "env", "exe")
             if ra["stamp"].get(k) != rb["stamp"].get(k)]

    if same:
        print("  bytes IDENTICAL" + (" (despite %s differing)" % "/".join(moved) if moved else ""))
        return 0
    print("  bytes DIFFER (same algorithm on both sides -- not a hash mix-up)")
    if moved:
        for k in moved:
            print("    %s= %s vs %s  -> %s"
                  % (k, ra["stamp"].get(k), rb["stamp"].get(k), WHY[k]))
        print("  Start the attribution there. It is not a tree/worktree effect: main")
        print("  and a fresh worktree were measured byte-identical (see this header).")
    else:
        print("    seed, spec, env and exe ALL match -- this is real nondeterminism.")
        print("    That has not been observed; capture both race.log files and dig.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
