#!/usr/bin/env python3
"""gen_levels.py -- build the auto track for a few seeds and keep each result
as a catalogue input under tools/trackgen_catalog/levels/seed<N>/ (gitignored).

Why several seeds: the biome layout (CITY, FIELDS, FOREST, INDUSTRIAL, ALPINE,
COAST, ORIENTAL, ALPTOWN) is laid out from the seed and there is no knob that
forces a biome, so one seed never shows every piece. The catalogue unions all
stored seeds and says, per piece, which seeds have it.

Each seed runs verify/geo_tags_identity.ps1 (the same launch the synthetic
identity gate uses: windowed, RT off, minimum graphics, no socket), which
writes re/assets/levels/level090. The files are copied out right after. The
REFERENCE seed 99991 is always built LAST so level090 ends as the reference
level (MODELS sha256 E2F1F33C...).

    python tools/trackgen_catalog/gen_levels.py                 # default seeds
    python tools/trackgen_catalog/gen_levels.py --seeds 1 2 3

Needs a DEV td5re.exe in the repo root (MESHTAG.BIN is dev-only), and
re/assets/geo/SELECTED.TXT moved aside (a selected place poisons the synthetic
build, see the round memory on SELECTED.TXT).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LEVEL = os.path.join(REPO, "re", "assets", "levels", "level090")
STORE = os.path.join(HERE, "levels")
FILES = ("MODELS.DAT", "MESHTAG.BIN", "TEXTURES.DAT", "GENSTAMP.TXT",
         "STRIP.DAT", "NETWORK.JSON")
REFERENCE = 99991
DEFAULT_SEEDS = [1, 2, 3, 7, 12345, 4172065417]


def build(seed, i, n, genwait):
    print(f"[{i}/{n}] seed {seed} building", flush=True)
    r = subprocess.run(["pwsh", "-NoProfile", "-File",
                        os.path.join(REPO, "verify", "geo_tags_identity.ps1"),
                        "-Arm", "synthetic", "-Tag", f"cat{seed}",
                        "-Seed", str(seed), "-NoControl",
                        "-GenWait", str(genwait)],
                       cwd=REPO, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    models = [l.strip() for l in out.splitlines() if "MODELS.DAT" in l]
    if not os.path.exists(os.path.join(LEVEL, "MODELS.DAT")):
        print(f"[{i}/{n}] seed {seed} FAIL (no MODELS.DAT)\n{out[-800:]}", flush=True)
        return False
    dst = os.path.join(STORE, f"seed{seed}")
    os.makedirs(dst, exist_ok=True)
    for f in FILES:
        s = os.path.join(LEVEL, f)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, f))
    rl = os.path.join(REPO, "log", f"idgate_synthetic_cat{seed}_race.log")
    if os.path.exists(rl):
        shutil.copy2(rl, os.path.join(dst, "race.log"))
    print(f"[{i}/{n}] seed {seed} PASS  {models[0] if models else ''}", flush=True)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", nargs="*", type=int, default=DEFAULT_SEEDS)
    ap.add_argument("--genwait", type=int, default=240,
                    help="seconds to wait for MODELS.DAT per seed (default 240)")
    ap.add_argument("--no-reference", action="store_true",
                    help="do not rebuild seed 99991 at the end")
    a = ap.parse_args()
    sel = os.path.join(REPO, "re", "assets", "geo", "SELECTED.TXT")
    if os.path.exists(sel):
        sys.exit("re/assets/geo/SELECTED.TXT is present: move it aside first "
                 "(it turns the synthetic build into a geo build).")
    seeds = [s for s in a.seeds if s != REFERENCE] + ([] if a.no_reference else [REFERENCE])
    ok = 0
    for i, s in enumerate(seeds, 1):
        ok += build(s, i, len(seeds), a.genwait)
    print(f"{ok}/{len(seeds)} seeds stored under {STORE}")
    return 0 if ok == len(seeds) else 1


if __name__ == "__main__":
    sys.exit(main())
