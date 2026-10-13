"""r1029x_route_search.py -- round 1029 X: rebuild the perturbed La Plata routes of verify/geo_n2_search.py with extra
generator knobs (verify/geo_n2_build.ps1 -Extra), so a knob's effect on the loop set is read route by route.

    python verify/r1029x_route_search.py <seed> <count> <tag> "TD5RE_GEO_LOOP_FORK_RAMP=1" [only1,only2,...]
    python verify/geo_n2_summ.py <tag>

The routes are the SAME waypoints geo_n2_search.py <seed> <count> builds (make_jobs), named <tag>NN. Each kept route has
its _route/ and race.log under verify/out/n2_routes/<tag>NN. Pairs found by the engine are the race.log lines
"side link: loops ... share departure spans" (two loop mouths) and "... paired with the real fork" (loop + fork).
"""
import concurrent.futures as cf
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_n2_search  # noqa: E402

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(job):
    name, pts, extra = job
    args = ["pwsh", "-NoProfile", "-File", os.path.join(WT, "verify", "geo_n2_build.ps1"), "-Name", name, "-Pts", pts]
    if extra:
        args += ["-Extra", extra]
    subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return name


def main():
    seed, count, tag, extra = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    only = set(sys.argv[5].split(",")) if len(sys.argv) > 5 and sys.argv[5] else None
    jobs = []
    for i, (_, pts) in enumerate(geo_n2_search.make_jobs(seed, count, "")):
        nn = "%02d" % i
        if only and nn not in only:
            continue
        jobs.append((tag + nn, pts, extra))
    with cf.ThreadPoolExecutor(6) as ex:
        for name in ex.map(build, jobs):
            print(name, flush=True)
    import geo_n2_summ
    geo_n2_summ.main(tag)
    for d in sorted(os.listdir(os.path.join(WT, "verify", "out", "n2_routes"))):
        if not d.startswith(tag):
            continue
        lg = os.path.join(WT, "verify", "out", "n2_routes", d, "race.log")
        if not os.path.exists(lg):
            continue
        for ln in open(lg, encoding="utf-8", errors="replace"):
            if "share departure spans" in ln or "paired with the real fork" in ln:
                print(d, ln.strip()[ln.find("side link"):] if "side link" in ln else ln.strip())


if __name__ == "__main__":
    main()
