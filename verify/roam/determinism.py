#!/usr/bin/env python
"""GEO FREE ROAM F1-T5: ROAMTILE determinism harness.

Builds the casco tiles with the dev exe several times and compares every output
file byte for byte (design 1.10, risk 4):

  A1  forward order, main thread                    (the reference)
  A2  forward order, main thread, a second process  (run to run)
  R   REVERSED tile order, main thread              (no state leaks between tiles)
  T   one worker thread, main FP env applied        (the F3 situation)
  N   one worker thread, main FP env NOT applied    (NEGATIVE CONTROL: shows the
                                                     harness can see an FP-env change;
                                                     reported, does not fail the run)
  G   one GLOBAL window build sliced per tile       (cross-check of the per-tile
                                                     windows: header input hash, STMP
                                                     text and trailer are masked, since
                                                     the hash legitimately depends on the
                                                     window the cache tiles were read for)

Usage (from anywhere):  python -I verify/roam/determinism.py [--root <project root>]
        [--exe td5re.exe] [--work <dir>] [--arms A1,A2,R,T,N,G] [--keep] [--scenery]

--scenery (roam F4a): the same arms over the SCENERY layer instead
(--RoamBuildScenery=casco from the ROAMTILE files on disk, SCENERY_<tx>_<tz>.BIN =
record table + MODELS records + the layer hash). G does not apply to scenery.

Exit code 0 when every non-control arm is byte identical to A1, 1 otherwise.
The exe is run with cwd = the project root (it reads re/assets/geo relative to it).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

TAGS = ["SPAN", "VERT", "TRI ", "BEDG", "EDGE", "NODE", "ARM ", "GRID", "SIGN", "NAME", "STMP"]
HDR = 64


PREFIX = "ROAMTILE_"
VERB = "--RoamBuildTiles=casco"


def run_arm(exe, root, out_dir, census, extra, log_path):
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    cmd = [exe, VERB, "--RoamOut=" + out_dir, "--RoamCensus=" + census] + extra
    t0 = time.time()
    with open(log_path, "wb") as lg:
        rc = subprocess.run(cmd, cwd=root, stdout=lg, stderr=subprocess.STDOUT).returncode
    return rc, time.time() - t0


def sections(buf):
    """[(tag, offset, bytes, count)] of a ROAMTILE image, or None when malformed."""
    if len(buf) < HDR + 4 or buf[:8] != b"TD5ROAM1":
        return None
    nsect = struct.unpack_from("<I", buf, 36)[0]     # magic 8, version, rules, hash, tx, tz, org x/z
    out = []
    for i in range(nsect):
        tag, off, nb, cnt = struct.unpack_from("<4sIII", buf, HDR + 16 * i)
        out.append((tag.decode("latin1"), off, nb, cnt))
    return out


def masked(buf):
    """The image with the window-dependent bytes zeroed: header input_hash, STMP, trailer."""
    if PREFIX != "ROAMTILE_":
        return buf
    b = bytearray(buf)
    b[16:20] = b"\0\0\0\0"
    for tag, off, nb, _ in sections(buf) or []:
        if tag == "STMP":
            b[off:off + nb] = b"\0" * nb
    b[-4:] = b"\0\0\0\0"
    return bytes(b)


def first_diff(a, b):
    n = min(len(a), len(b))
    i = 0
    # chunked scan: fast on identical-prefix files
    step = 4096
    while i < n and a[i:i + step] == b[i:i + step]:
        i += step
    i = min(i, n)
    while i < n and a[i] == b[i]:
        i += 1
    return i


def where(buf, off):
    for tag, so, nb, _ in sections(buf) or []:
        if so <= off < so + nb:
            return "%s+%d" % (tag, off - so)
    return "header/table" if off < HDR + 16 * len(TAGS) else "padding/trailer"


def compare(ref_dir, arm_dir, mask):
    ref = sorted(f for f in os.listdir(ref_dir) if f.startswith(PREFIX))
    got = set(f for f in os.listdir(arm_dir) if f.startswith(PREFIX))
    same = 0
    diffs = []
    for f in ref:
        if f not in got:
            diffs.append((f, "missing", 0))
            continue
        with open(os.path.join(ref_dir, f), "rb") as fa, open(os.path.join(arm_dir, f), "rb") as fb:
            a, b = fa.read(), fb.read()
        if mask:
            a, b = masked(a), masked(b)
        if a == b:
            same += 1
        else:
            off = first_diff(a, b)
            diffs.append((f, "differs at %s (len %d vs %d)" % (where(a, off) if PREFIX == "ROAMTILE_" else "byte %d" % off,
                                                                  len(a), len(b)), off))
    extra = sorted(got - set(ref))
    return len(ref), same, diffs, extra


ARMS = {
    "A1": ([], False, "forward, main thread"),
    "A2": ([], False, "forward, main thread, second process"),
    "R": (["--RoamOrder=rev"], False, "reversed tile order, main thread"),
    "T": (["--RoamThread=1"], False, "worker thread, main FP env applied"),
    "N": (["--RoamThread=2"], False, "worker thread, FP env NOT applied (negative control)"),
    "G": (["--RoamGlobal=1"], True, "one global window sliced per tile (hash/STMP/trailer masked)"),
}
CONTROLS = {"N"}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.abspath(os.path.join(here, "..", "..")))
    ap.add_argument("--exe", default="td5re.exe")
    ap.add_argument("--work", default=None)
    ap.add_argument("--arms", default="A1,A2,R,T,N,G")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--scenery", action="store_true", help="roam F4a: the SCENERY layer (SCENERY_*.BIN)")
    a = ap.parse_args()
    global PREFIX, VERB
    if a.scenery:
        PREFIX = "SCENERY_"
        VERB = "--RoamBuildScenery=casco"
        a.arms = ",".join(x for x in a.arms.split(",") if x != "G")
    root = a.root
    exe = a.exe if os.path.isabs(a.exe) else os.path.join(root, a.exe)
    work = a.work or os.path.join(root, "log", "roam_determinism" + ("_scenery" if a.scenery else ""))
    os.makedirs(work, exist_ok=True)
    arms = [x for x in a.arms.split(",") if x]
    if "A1" not in arms:
        arms.insert(0, "A1")
    print("exe   :", exe)
    print("root  :", root)
    print("work  :", work)
    failed = False
    results = []
    for name in arms:
        extra, mask, desc = ARMS[name]
        out_dir = os.path.join(work, name)
        rc, secs = run_arm(exe, root, out_dir, os.path.join(work, "census_%s.txt" % name), extra,
                           os.path.join(work, "run_%s.log" % name))
        nfiles = len([f for f in os.listdir(out_dir) if f.startswith(PREFIX)])
        total = sum(os.path.getsize(os.path.join(out_dir, f)) for f in os.listdir(out_dir) if f.startswith(PREFIX))
        print("\n[%s] %s: exit %d, %d file(s), %.1f MB, %.1f s" % (name, desc, rc, nfiles, total / 1e6, secs))
        if rc != 0 or nfiles == 0:
            print("  ARM FAILED TO RUN (see %s)" % os.path.join(work, "run_%s.log" % name))
            failed = failed or name not in CONTROLS
            results.append((name, None))
            continue
        if name == "A1":
            results.append((name, "reference"))
            continue
        n, same, diffs, extra_f = compare(os.path.join(work, "A1"), out_dir, mask)
        ok = (not diffs) and (not extra_f)
        verdict = "IDENTICAL" if ok else "DIFFERENT"
        if name in CONTROLS:
            verdict += " (control: a difference here is expected evidence, not a failure)" if not ok else \
                       " (control did NOT differ: the FP env rule is not observable on this data)"
        print("  vs A1: %d/%d files byte identical%s -> %s" % (same, n, " (masked compare)" if mask else "", verdict))
        for f, why, _ in diffs[:8]:
            print("    %s: %s" % (f, why))
        if len(diffs) > 8:
            print("    ... %d more" % (len(diffs) - 8))
        if extra_f:
            print("    extra files:", extra_f[:5])
        results.append((name, ok))
        if not ok and name not in CONTROLS:
            failed = True
    print("\nSUMMARY")
    for name, ok in results:
        print("  %-3s %s" % (name, "reference" if ok == "reference" else ("n/a" if ok is None else ("identical" if ok else "DIFFERENT"))))
    print("RESULT:", "FAIL" if failed else "PASS")
    if not a.keep:
        for name in arms:
            if name != "A1":
                shutil.rmtree(os.path.join(work, name), ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
