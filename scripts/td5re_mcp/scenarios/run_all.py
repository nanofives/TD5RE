"""run_all.py -- discover + run every scenario, aggregate a report.

Each scenario runs in its own subprocess with a fresh game instance (the
scenario launches/quits the game itself), so one wedged run can't poison the
next. Aggregates to stdout + log/scenario_report.md; exit 0 only if every
scenario passed (mirrors the selftest contract).

Usage: python scenarios/run_all.py [name ...]   (default: all)
"""
from __future__ import annotations

import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
REPORT = REPO_ROOT / "log" / "scenario_report.md"
PER_SCENARIO_TIMEOUT = 420          # seconds; generous, scenarios poll states


def discover(names: list[str]) -> list[Path]:
    all_scripts = sorted(p for p in HERE.glob("*.py")
                         if not p.name.startswith(("_", "run_all")))
    if not names:
        return all_scripts
    picked = []
    for n in names:
        p = HERE / (n if n.endswith(".py") else n + ".py")
        if not p.exists():
            print(f"unknown scenario '{n}' (known: "
                  + ", ".join(x.stem for x in all_scripts) + ")")
            sys.exit(2)
        picked.append(p)
    return picked


def main() -> int:
    scripts = discover(sys.argv[1:])
    rows = []
    t_suite = time.time()
    for script in scripts:
        print(f"=== {script.stem} ===", flush=True)
        t0 = time.time()
        try:
            res = subprocess.run([sys.executable, str(script)],
                                 cwd=str(REPO_ROOT), timeout=PER_SCENARIO_TIMEOUT)
            status = "PASS" if res.returncode == 0 else "FAIL"
        except subprocess.TimeoutExpired:
            status = "TIMEOUT"
        rows.append((script.stem, status, time.time() - t0))
        time.sleep(1.0)          # let sockets/process teardown settle

    n_pass = sum(1 for _, st, _ in rows if st == "PASS")
    verdict_line = (f"Scenarios: **{n_pass}/{len(rows)} PASS** — "
                    f"total {time.time() - t_suite:.0f} s — "
                    f"{datetime.now():%Y-%m-%d %H:%M}")

    REPORT.parent.mkdir(exist_ok=True)
    with open(REPORT, "w", encoding="utf-8") as f:
        f.write("# TD5RE scenario report\n\n" + verdict_line + "\n\n")
        f.write("| scenario | status | seconds |\n|---|---|---|\n")
        for name, status, secs in rows:
            f.write(f"| {name} | {status} | {secs:.0f} |\n")
        f.write("\nPer-check detail is in each scenario's stdout; "
                "framedump evidence in log/scenarios/.\n")

    print("\n" + verdict_line)
    for name, status, secs in rows:
        print(f"  {status:7s} {name} ({secs:.0f}s)")
    print(f"report: {REPORT}")
    return 0 if n_pass == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
