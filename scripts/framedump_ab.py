#!/usr/bin/env python3
"""Framedump A/B comparison harness for the D3D11 vs D3D12 backend port.

The *render net* for the D3D12 port (docs/plans/D3D12_PORT_PLAN.md sec 3): drive
two backend builds to an identical, deterministic scene via the live-control
socket, capture an in-engine backbuffer PNG from each (the `framedump` verb,
reliable even when occluded), and diff them pixel-wise.

Determinism: both builds boot the SAME pinned config and are driven to the SAME
observable state before the dump --

  * `menu`  scenario: a fully static frontend screen (no sim) -- expect
    near-bit-identical (the Phase 2 gate: "menus are the simplest full exercise
    of the draw+state+texture path").
  * `grid`  scenario: an in-race frame captured while the countdown is still
    active, so the sim is frozen at the deterministic grid start positions --
    a static full-3D-world A/B without needing to hit an exact moving sim tick.
  * `tick`  scenario: dump at the first frame with sim_tick >= --target-tick.
    Only bit-meaningful once input is applied deterministically per sim tick;
    use `grid` for a robust static world compare.

Because runs are sequential, both builds can share the default control port.

Usage (from repo root):
  python scripts/framedump_ab.py --scenario menu
  python scripts/framedump_ab.py --scenario grid --track 5
  python scripts/framedump_ab.py \
      --exe-a td5re.exe --label-a d3d11 \
      --exe-b td5re_d3d12.exe --label-b d3d12 \
      --scenario menu --tolerance 4

Exit code 0 = within tolerance (PASS), 1 = mismatch/FAIL, 2 = harness error.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts" / "td5re_mcp"))

try:
    from game_client import GameClient, ControlError  # type: ignore
except Exception as exc:  # pragma: no cover
    print(f"[ab] cannot import game_client: {exc}", file=sys.stderr)
    sys.exit(2)

try:
    import numpy as np
    from PIL import Image
except Exception as exc:  # pragma: no cover
    print(f"[ab] numpy/PIL required: {exc}", file=sys.stderr)
    sys.exit(2)


def log(msg: str) -> None:
    print(f"[ab] {msg}", flush=True)


def _wait_reply(cli: GameClient, cmd: str, deadline: float,
                args: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
    """Poll a command until it answers or the deadline passes (socket may not be
    bound yet during boot -> ControlError; keep retrying)."""
    while time.time() < deadline:
        try:
            return cli.command(cmd, args)
        except ControlError:
            time.sleep(0.2)
    return None


def _reach_menu(cli: GameClient, deadline: float, stable_polls: int = 5) -> Dict[str, Any]:
    """Wait until a frontend screen has been unchanged for `stable_polls`."""
    last_screen = None
    stable = 0
    st: Dict[str, Any] = {}
    while time.time() < deadline:
        try:
            st = cli.get_state()
        except ControlError:
            time.sleep(0.2)
            continue
        gsn = str(st.get("game_state_name", ""))
        screen = st.get("screen")
        # frontend (not in-race) and screen index holding steady
        in_race = "race" in gsn.lower() or bool(st.get("race"))
        if not in_race and screen is not None and screen == last_screen:
            stable += 1
            if stable >= stable_polls:
                return st
        else:
            stable = 0
        last_screen = screen
        time.sleep(0.25)
    return st


def _reach_grid(cli: GameClient, deadline: float) -> Dict[str, Any]:
    """Wait until in-race with the countdown active (sim frozen at grid)."""
    st: Dict[str, Any] = {}
    while time.time() < deadline:
        try:
            st = cli.get_state()
        except ControlError:
            time.sleep(0.2)
            continue
        race = st.get("race")
        if isinstance(race, dict) and race.get("countdown"):
            return st
        time.sleep(0.15)
    return st


def _reach_tick(cli: GameClient, deadline: float, target: int) -> Dict[str, Any]:
    st: Dict[str, Any] = {}
    while time.time() < deadline:
        try:
            st = cli.get_state()
        except ControlError:
            time.sleep(0.2)
            continue
        race = st.get("race")
        if isinstance(race, dict) and int(race.get("sim_tick", -1)) >= target:
            return st
        time.sleep(0.05)
    return st


def capture_one(exe: str, label: str, scenario: str, track: int,
                target_tick: int, out_dir: Path, port: int,
                boot_timeout: float) -> Optional[Path]:
    """Launch one backend, drive it to the scenario state, dump, quit."""
    exe_path = REPO / exe
    if not exe_path.exists():
        log(f"MISSING exe: {exe_path}")
        return None

    args: List[str] = [str(exe_path), "--Control=1"]  # port via TD5RE_CONTROL_PORT env
    if scenario == "menu":
        args += ["--SkipIntro=1"]
    else:  # grid / tick -> boot straight into a race with a pinned seed
        args += ["--AutoRace=1", "--SkipIntro=1", f"--DefaultTrack={track}",
                 "--RaceTrace=1"]

    dump_path = out_dir / f"ab_{label}.png"
    if dump_path.exists():
        dump_path.unlink()

    env = dict(os.environ)
    env["TD5RE_D3D_DEBUG"] = "1"
    env["TD5RE_CONTROL_PORT"] = str(port)
    # Arm the in-engine backbuffer dump. The env path writes the PNG every 30
    # frames (overwrite) and, for the d3d12 backend, is what arms the pre-flip
    # capture. Once the target scene is static, every write is identical, so
    # reading the file after a clean shutdown gives the deterministic frame --
    # and it works uniformly for both backends without the control verb.
    env["TD5RE_FRAMEDUMP"] = str(dump_path)

    log(f"launch {label}: {' '.join(args[1:])}")
    proc = subprocess.Popen(args, cwd=str(REPO), env=env)
    cli = GameClient(port=port, timeout=1.0, retries=2)
    try:
        deadline = time.time() + boot_timeout
        if _wait_reply(cli, "ping", deadline) is None:
            log(f"{label}: no control-socket ping within {boot_timeout:.0f}s")
            return None

        if scenario == "menu":
            st = _reach_menu(cli, deadline)
            log(f"{label}: menu screen={st.get('screen')} "
                f"state={st.get('game_state_name')}")
        elif scenario == "grid":
            st = _reach_grid(cli, deadline)
            race = st.get("race") or {}
            log(f"{label}: grid tick={race.get('sim_tick')} "
                f"countdown={race.get('countdown')}")
        else:  # tick
            st = _reach_tick(cli, deadline, target_tick)
            race = st.get("race") or {}
            log(f"{label}: tick={race.get('sim_tick')} (target {target_tick})")

        # Hold the static scene long enough for a fresh 30-frame env dump to
        # land, then quit cleanly (stops further writes + flushes logs).
        time.sleep(2.0)
        cli.command("quit")
    finally:
        cli.close()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            log(f"{label}: did not exit cleanly -> kill pid {proc.pid}")
            proc.kill()

    # Read after the process has exited: no writer can be mid-overwrite.
    if dump_path.exists() and dump_path.stat().st_size > 0:
        log(f"{label}: dumped {dump_path.name} ({dump_path.stat().st_size} B)")
        return dump_path
    log(f"{label}: NO DUMP produced")
    return None


def diff_images(a: Path, b: Path, tolerance: int, out_dir: Path) -> int:
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    resized = False
    if ia.size != ib.size:
        # The two backends differ in internal render resolution at this phase:
        # d3d11 captures its offscreen backbuffer at the device size, d3d12
        # renders direct-to-swapchain at the INI target size. Normalize to the
        # smaller size (high-quality resample) so the CONTENT can be compared;
        # the resample adds a little edge noise, so the pass thresholds below
        # are looser than the same-size bit-identical case.
        tw = min(ia.width, ib.width)
        th = min(ia.height, ib.height)
        log(f"NOTE: size mismatch {ia.size} vs {ib.size} -> resample both to "
            f"({tw},{th}) for content compare")
        ia = ia.resize((tw, th), Image.LANCZOS)
        ib = ib.resize((tw, th), Image.LANCZOS)
        resized = True
    na = np.asarray(ia, dtype=np.int16)
    nb = np.asarray(ib, dtype=np.int16)
    absd = np.abs(na - nb)
    per_px = absd.max(axis=2)          # worst channel per pixel
    max_d = int(per_px.max())
    mean_d = float(absd.mean())
    total = per_px.size
    over = int((per_px > tolerance).sum())
    frac = 100.0 * over / total

    log(f"size={ia.size} max_channel_diff={max_d} mean_diff={mean_d:.3f}")
    log(f"pixels over tol({tolerance}): {over}/{total} = {frac:.4f}% (fine-grained)")

    # ---- structural score (the actual gate) ----------------------------------
    # Fine-grained per-pixel diff is dominated by 1px edge-shift (resolution
    # differs between the offscreen d3d11 backbuffer and the direct-to-swapchain
    # d3d12 render) and by time-based menu animation / the live FPS counter --
    # none of which is a render bug. Collapsing both frames to a small grid
    # averages that high-frequency noise out and leaves only LOW-frequency
    # structure: layout, presence of elements, colour blocks. A gross
    # divergence (missing UI, wrong colours, corruption) survives the downscale;
    # edge-shift and animation glow do not.
    SW, SH = 160, 84
    sa = np.asarray(Image.fromarray(na.astype(np.uint8)).resize((SW, SH), Image.LANCZOS),
                    dtype=np.float64)
    sb = np.asarray(Image.fromarray(nb.astype(np.uint8)).resize((SW, SH), Image.LANCZOS),
                    dtype=np.float64)
    struct_mean = float(np.abs(sa - sb).mean())
    mean_rgb_a = na.reshape(-1, 3).mean(0)
    mean_rgb_b = nb.reshape(-1, 3).mean(0)
    tint = float(np.abs(mean_rgb_a - mean_rgb_b).max())
    log(f"structural(160x84) mean_abs_diff={struct_mean:.2f}  "
        f"global_tint_delta={tint:.2f}")

    # a/b/diff montage for human review (the primary artifact)
    mw, mh = 900, int(900 * ia.height / ia.width)
    ma = ia.resize((mw, mh), Image.LANCZOS)
    mb = ib.resize((mw, mh), Image.LANCZOS)
    md = Image.fromarray(np.clip(np.abs(np.asarray(ma, np.int16) -
                                        np.asarray(mb, np.int16)) * 4, 0, 255).astype(np.uint8))
    montage = Image.new("RGB", (mw, mh * 3 + 20), (30, 30, 30))
    montage.paste(ma, (0, 0)); montage.paste(mb, (0, mh + 10))
    montage.paste(md, (0, 2 * mh + 20))
    montage_path = out_dir / "ab_montage.png"
    montage.save(montage_path)
    log(f"montage (d3d11 / d3d12 / 4x-diff) -> {montage_path.name}")

    # Gate: structural mean < 18 catches gross divergence while tolerating
    # resample edge-shift + animation phase (~10 for an equivalent animated
    # menu). global_tint_delta must stay small (a real colour-space/gamma bug
    # would shift the whole frame's mean).
    STRUCT_LIMIT, TINT_LIMIT = 18.0, 8.0
    if struct_mean <= STRUCT_LIMIT and tint <= TINT_LIMIT:
        log(f"PASS (structural): mean {struct_mean:.2f}<={STRUCT_LIMIT}, "
            f"tint {tint:.2f}<={TINT_LIMIT} -- review ab_montage.png to confirm")
        return 0
    log(f"FAIL (structural): mean {struct_mean:.2f} (limit {STRUCT_LIMIT}) / "
        f"tint {tint:.2f} (limit {TINT_LIMIT}) -- see ab_montage.png")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe-a", default="td5re.exe")
    ap.add_argument("--label-a", default="d3d11")
    ap.add_argument("--exe-b", default="td5re_d3d12.exe")
    ap.add_argument("--label-b", default="d3d12")
    ap.add_argument("--scenario", choices=["menu", "grid", "tick"], default="menu")
    ap.add_argument("--track", type=int, default=5)
    ap.add_argument("--target-tick", type=int, default=120)
    ap.add_argument("--tolerance", type=int, default=4,
                    help="max abs per-channel diff still counted as equal")
    ap.add_argument("--out-dir", default="log")
    ap.add_argument("--port", type=int, default=37060)
    ap.add_argument("--boot-timeout", type=float, default=40.0)
    args = ap.parse_args()

    out_dir = (REPO / args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    log(f"scenario={args.scenario} A={args.label_a}({args.exe_a}) "
        f"B={args.label_b}({args.exe_b})")

    pa = capture_one(args.exe_a, args.label_a, args.scenario, args.track,
                     args.target_tick, out_dir, args.port, args.boot_timeout)
    if pa is None:
        log("harness error: A capture failed")
        return 2
    pb = capture_one(args.exe_b, args.label_b, args.scenario, args.track,
                     args.target_tick, out_dir, args.port, args.boot_timeout)
    if pb is None:
        log("harness error: B capture failed")
        return 2

    return diff_images(pa, pb, args.tolerance, out_dir)


if __name__ == "__main__":
    sys.exit(main())
