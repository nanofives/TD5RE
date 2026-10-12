"""geo_n2_search.py -- round 1022 N2: build N random La Plata routes (perturbed copies of two known ones) in private
run dirs through verify/geo_n2_build.ps1, then summarise each route: the real forks (kind + side) and the block-loop
candidates / built loops.  A candidate printed with flag 1 is a loop that shares a real fork's window
(TD5RE_GEO_LOOP_FORK_BESIDE).

    python verify/geo_n2_search.py <seed> <count> <tag>      # builds verify/out/n2_routes/<tag>NN
    python verify/geo_n2_summ.py <tag>                       # re-print the summary of kept routes

Each route keeps its _route/ (race it with verify/geo_fork_validate.ps1 -RouteDir ...) and its race.log.
"""
import concurrent.futures as cf
import os
import random
import subprocess
import sys

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = [(-34.92299, -57.97785), (-34.92444, -57.94996), (-34.92299, -57.93855)]     # the 10-10 route (fingerprint 4B56DECB)
B = [(-34.921975, -57.966677), (-34.920756, -57.942802)]                         # the 10-11 19:56 route (fingerprint 7820EC8E)


def make_jobs(seed, count, tag, spread=0.004):
    random.seed(seed)
    jobs = []
    for i in range(count):
        base = random.choice([A, B, A])
        pts = [(la + random.uniform(-spread, spread), lo + random.uniform(-spread, spread)) for la, lo in base]
        if random.random() < 0.25:
            pts = pts[::-1]
        jobs.append(("%s%02d" % (tag, i), ";".join("%.5f,%.5f" % p for p in pts)))
    return jobs


def build(job):
    name, pts = job
    subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(WT, "verify", "geo_n2_build.ps1"), "-Name", name, "-Pts", pts],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return name, pts


def main():
    seed, count, tag = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    with cf.ThreadPoolExecutor(6) as ex:
        for name, pts in ex.map(build, make_jobs(seed, count, tag)):
            print(name, pts, flush=True)
    sys.argv = ["geo_n2_summ.py", tag]
    import geo_n2_summ
    geo_n2_summ.main(tag)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
