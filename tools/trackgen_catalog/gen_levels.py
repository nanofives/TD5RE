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
# Seed 1 is left out on purpose: on 2026-11-08 its build never wrote
# MODELS.DAT within 15 minutes (the game loaded the meshes, nothing reached
# disk); every other seed here writes in under a minute.
DEFAULT_SEEDS = [2, 3, 7, 12345, 4172065417]


def build(seed, i, n, genwait, night=False):
    tag = f"cat{seed}" + ("n" if night else "")
    print(f"[{i}/{n}] seed {seed}{' night' if night else ''} building", flush=True)
    # The identity script clears every TD5RE_* variable, so a knob can only go
    # in through its -Extra hashtable, which needs -Command rather than -File.
    script = os.path.join(REPO, "verify", "geo_tags_identity.ps1")
    extra = "@{TD5RE_AUTOTRACK_NIGHT='1'}" if night else "@{}"
    r = subprocess.run(["pwsh", "-NoProfile", "-Command",
                        f"& '{script}' -Arm synthetic -Tag {tag} -Seed {seed} "
                        f"-NoControl -GenWait {genwait} -Extra {extra}"],
                       cwd=REPO, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    models = [l.strip() for l in out.splitlines() if "MODELS.DAT" in l]
    if not os.path.exists(os.path.join(LEVEL, "MODELS.DAT")):
        print(f"[{i}/{n}] seed {seed} FAIL (no MODELS.DAT)\n{out[-800:]}", flush=True)
        return False
    dst = os.path.join(STORE, f"seed{seed}" + ("_night" if night else ""))
    os.makedirs(dst, exist_ok=True)
    for f in FILES:
        s = os.path.join(LEVEL, f)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, f))
    rl = os.path.join(REPO, "log", f"idgate_synthetic_{tag}_race.log")
    if os.path.exists(rl):
        shutil.copy2(rl, os.path.join(dst, "race.log"))
    print(f"[{i}/{n}] seed {seed} PASS  {models[0] if models else ''}", flush=True)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", nargs="*", type=int, default=DEFAULT_SEEDS)
    ap.add_argument("--genwait", type=int, default=240,
                    help="seconds to wait for MODELS.DAT per seed (default 240)")
    ap.add_argument("--night", type=int, nargs="*", default=[2],
                    help="seeds to ALSO build at night (TD5RE_AUTOTRACK_NIGHT=1); default 2")
    ap.add_argument("--no-reference", action="store_true",
                    help="do not rebuild seed 99991 at the end")
    a = ap.parse_args()
    sel = os.path.join(REPO, "re", "assets", "geo", "SELECTED.TXT")
    if os.path.exists(sel):
        sys.exit("re/assets/geo/SELECTED.TXT is present: move it aside first "
                 "(it turns the synthetic build into a geo build).")
    seeds = ([(s, False) for s in a.seeds if s != REFERENCE] + [(s, True) for s in a.night]
             + ([] if a.no_reference else [(REFERENCE, False)]))
    ok = 0
    for i, (s, night) in enumerate(seeds, 1):
        ok += build(s, i, len(seeds), a.genwait, night)
    print(f"{ok}/{len(seeds)} seeds stored under {STORE}")
    return 0 if ok == len(seeds) else 1


if __name__ == "__main__":
    sys.exit(main())
