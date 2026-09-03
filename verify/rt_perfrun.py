"""[RT WINDOW 2026-09-03] Frame-rate probe: drive one td5re.exe race over the control
socket and summarise the PROFILE / RENDERSTAT lines of the measurement window.

  python verify/rt_perfrun.py --tag A --quality 1 --track 60 --secs 30       --env TD5RE_FRAME_CAP=0 --env TD5RE_RT_DIAG=1       --arg=--Width=2560 --arg=--Height=1351 --arg=--ViewDistance=100 --arg=--VSync=0
      [--nothrottle] [--dump out.png] [--exe other.exe] [--wt <worktree>] [--port N]

Numbers that matter: run with TD5RE_FRAME_CAP=0 and --VSync=0 (else you measure
the cap), at the resolution you care about, and compare SAME views: --nothrottle
holds the start-line view; with throttle the car drives blind (it usually hits a
wall around span 110 on seed 20260901, which is fine for A/B as long as both
sides do). --quality 0/1 = LIGHTING QUALITY LOW/HIGH. Never steals focus.
Requires [Logging] Enabled=1 Profile=1 Engine=1 in the worktree td5re.ini.
Outputs <tag>.engine.log / <tag>.race.log next to this script.
"""
import argparse, json, os, re, shutil, socket, statistics, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--quality", type=int, default=0)
ap.add_argument("--track", type=int, default=60)
ap.add_argument("--secs", type=float, default=30)
ap.add_argument("--exe", default="td5re.exe")
ap.add_argument("--wt", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap.add_argument("--port", type=int, default=37171)
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--opp", type=int, default=5)
ap.add_argument("--timeout", type=float, default=400)
ap.add_argument("--nothrottle", action="store_true")
ap.add_argument("--arg", action="append", default=[], help="extra --Key=N CLI overrides")
ap.add_argument("--dump", default="", help="PNG path: arm capture and framedump at the end of the window")
a = ap.parse_args()

out_dir = os.path.dirname(os.path.abspath(__file__))
env = {k: v for k, v in os.environ.items() if not k.startswith("TD5RE_")}
env["TD5RE_CONTROL_PORT"] = str(a.port)
env["TD5RE_WINDOW_TITLE"] = "TD5RE perf %s" % a.tag
for kv in a.env:
    k, v = kv.split("=", 1); env[k] = v
if a.dump:
    env["TD5RE_D3D12_CAPTURE"] = "1"

log = os.path.join(a.wt, "log", "engine.log")
race_log = os.path.join(a.wt, "log", "race.log")
for f in (log,):
    for _ in range(20):
        try:
            if os.path.exists(f): os.remove(f)
            break
        except OSError: time.sleep(0.5)

args = [os.path.join(a.wt, a.exe), "--AutoRace=1", "--SkipIntro=1", "--Control=1",
        "--Quality=%d" % a.quality, "--DefaultTrack=%d" % a.track,
        "--DefaultOpponents=%d" % a.opp] + list(a.arg)
t0 = time.time()
p = subprocess.Popen(args, cwd=a.wt, env=env)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2.0)
def cmd(name, args=None):
    msg = json.dumps({"cmd": name, "args": args or {}}).encode()
    for _ in range(3):
        try:
            s.sendto(msg, ("127.0.0.1", a.port))
            return json.loads(s.recvfrom(65535)[0].decode())
        except Exception:
            continue
    return None

state = None
while time.time() - t0 < a.timeout and p.poll() is None:
    r = cmd("get_state")
    if r:
        state = r
        nm = str(r.get("game_state_name", "")).upper()
        race = r.get("race") or {}
        if "RACE" in nm and "PRE" not in nm and not race.get("countdown", True):
            break
    time.sleep(1.0)
else:
    print("TIMEOUT/exit waiting for RACE; last=%s exit=%s" % (json.dumps(state)[:300] if state else None, p.poll()))
    if p.poll() is None: cmd("quit"); time.sleep(5)
    if p.poll() is None: p.kill()
    sys.exit(2)
t_race = time.time()
print("in race after %.1fs: %s" % (t_race - t0, json.dumps(state)[:200]))
time.sleep(2.0)
if not a.nothrottle:
    print("throttle:", cmd("hold_action", {"slot": 0, "action": "throttle", "frames": 0}))
t_meas0 = time.time()
time.sleep(a.secs)
st = cmd("get_state", {"racers": 1})
print("end state:", json.dumps(st)[:400])
if a.dump:
    rel = "log/perf_ctrl_frame.png"
    full = os.path.join(a.wt, rel)
    if os.path.exists(full): os.remove(full)
    print("framedump ->", cmd("framedump", {"path": rel}))
    for _ in range(40):
        if os.path.exists(full) and os.path.getsize(full) > 0:
            time.sleep(1.0); shutil.copyfile(full, a.dump); print("saved dump", a.dump); break
        time.sleep(0.5)
cmd("quit")
for _ in range(40):
    if p.poll() is not None: break
    time.sleep(0.5)
if p.poll() is None:
    print("force kill"); p.kill()
time.sleep(1.0)
dst = os.path.join(out_dir, "%s.engine.log" % a.tag)
shutil.copyfile(log, dst)
try: shutil.copyfile(race_log, os.path.join(out_dir, "%s.race.log" % a.tag))
except Exception: pass

# --- summarize PROFILE lines that fall in the measurement window -------------
# engine.log lines carry a timestamp prefix; simpler: take the LAST N profile
# lines where N ~ secs (1 line/s), dropping the last 1 (quit frame).
lines = open(dst, encoding="utf-8", errors="replace").read().splitlines()
prof = [l for l in lines if "PROFILE (ms avg/max)" in l and "frame=" in l]
rs = [l for l in lines if "RENDERSTAT" in l]
n = int(a.secs) - 2
sel = prof[-(n + 1):-1] if len(prof) > n + 1 else prof
fps = []; zones = {}
for l in sel:
    m = re.search(r"frame=([\d.]+)/([\d.]+) fps=(\d+)", l)
    if m: fps.append(float(m.group(3)))
    for z, avg, mx in re.findall(r" (\w+)=([\d.]+)/([\d.]+)", l):
        zones.setdefault(z, []).append((float(avg), float(mx)))
print("=== %s  quality=%d track=%d  profile lines used=%d" % (a.tag, a.quality, a.track, len(sel)))
if fps:
    print("fps  median=%.0f  min=%.0f  max=%.0f" % (statistics.median(fps), min(fps), max(fps)))
for z, v in zones.items():
    if z == "frame": continue
    avgs = [x[0] for x in v]; mxs = [x[1] for x in v]
    print("  %-14s avg(median)=%6.2f ms   max(median)=%6.2f  max(worst)=%6.2f" % (z, statistics.median(avgs), statistics.median(mxs), max(mxs)))
for l in rs[-3:]:
    print(" ", l[l.find("RENDERSTAT"):])
print("saved", dst)
