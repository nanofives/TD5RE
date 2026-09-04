"""verify/span_capture.py -- drive a generated (or any) track under the control socket and
frame-dump at named spans. Used for the PENDING TO TEST autotrack rounds (A1..A6 in
docs/plans/PENDING_TRIAGE_2026-09-02.md): a row's CONFIRM text names spans; this script
launches the dev exe straight into the race, holds throttle (LaneAssist keeps the car on
the road), captures a PNG the moment the player's span reaches each target, quits cleanly
and prints the trackgen guard/audit lines from race.log.

  python verify/span_capture.py <outdir> [seed=99991] [start_span=51] [targets=66,75,...]
  env A1_PREFIX=<name>       file prefix (default a1)
  env TD5RE_CAM_TOPDOWN=9000 TD5RE_CAM_TOPDOWN_BACKDIV=2   oblique top-down instead of chase cam
  env TD5RE_AUTOTRACK_PARKS=0 etc. pass straight through to the game

Gotchas learned 2026-09-04:
  - hold_action expires after 60 frames unless frames=0.
  - The window is pushed to HWND_BOTTOM so the run never steals focus.
  - The exe is the one at the repo root above this script, so run it from the tree
    whose build you want to test.
  - CAMERA LIMIT: the top-down camera follows the CAR'S HEADING, so it always looks
    along the road being driven. It can never look DOWN a side street, which means
    it cannot answer "how deep does this perpendicular street run". Altitude does not
    help; 9000 clips through tall buildings, 6000 usually clears, 4500 clears the
    worst but crops the far side of a roadside gap out of frame. Depth-of-side-street
    questions need a camera that can be aimed independently, not this one.
  - StartSpanOffset SNAPS FORWARD: --StartSpanOffset=85 spawned the car at span 100,
    already past a gap at 94-96. Start earlier and drive in.
  - The 0.6 s settle after each framedump costs several spans at 130k+ speed, so
    tightly-spaced targets overshoot. Space targets out, or accept the drift (the
    filename records the span actually reached).
"""
import os, sys, time, subprocess, ctypes, json, re
WT=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
S=sys.argv[1]; PORT=int(os.environ.get("TD5RE_CONTROL_PORT","37072")); TITLE="TD5RE-a1-%d"%PORT; SEED=sys.argv[2] if len(sys.argv)>2 else "99991"
START=int(sys.argv[3]) if len(sys.argv)>3 else 51
TARGETS=[int(x) for x in (sys.argv[4].split(',') if len(sys.argv)>4 else "66,75,95,115,145,150,160,311,358,361".split(','))]
sys.path.insert(0, os.path.join(WT,"scripts","td5re_mcp")); from game_client import GameClient
# Autotrack knobs default ON for the A-rounds but ALWAYS yield to the environment.
# dict(os.environ, KEY=val) lets the kwarg WIN, so a forced kwarg here silently
# overrides the caller -- which is exactly the bug this replaces. Knobs not listed
# (TUNNELS, BRANCHES, R8_*, R9_*, CAM_*) already pass through untouched via os.environ.
def knob(name, default): return os.environ.get(name, default)
env=dict(os.environ, TD5RE_CONTROL_PORT=str(PORT), TD5RE_WINDOW_TITLE=TITLE, TD5RE_D3D12_CAPTURE="1",
         TD5RE_AUTOTRACK_SEED=SEED,
         TD5RE_AUTOTRACK_SCENERY=knob("TD5RE_AUTOTRACK_SCENERY","1"),
         TD5RE_AUTOTRACK_BRIDGES=knob("TD5RE_AUTOTRACK_BRIDGES","1"),
         TD5RE_AUTOTRACK_PARKS  =knob("TD5RE_AUTOTRACK_PARKS",  "1"))
for f in ("race.log","engine.log"):
    p=os.path.join(WT,"log",f)
    if os.path.exists(p):
        try: os.replace(p, p+".prev")
        except OSError: pass
args=[os.path.join(WT,"td5re.exe"),"--SkipIntro=1","--AutoRace=1","--DefaultTrack=60",f"--StartSpanOffset={START}","--DefaultOpponents=0","--Control=1","--LaneAssist=1"]
proc=subprocess.Popen(args, cwd=WT, env=env); print("pid",proc.pid,"seed",SEED,"start",START,"targets",TARGETS)
u=ctypes.windll.user32; back=False
def send_back():
    h=u.FindWindowW(None, TITLE)
    if h: u.SetWindowPos(h,1,0,0,0,0,0x0001|0x0002|0x0010); return True
    return False
c=GameClient(port=PORT); t0=time.time(); st=None
while time.time()-t0<240:
    if proc.poll() is not None: print("EXITED EARLY",proc.returncode); sys.exit(1)
    if not back: back=send_back()
    try: st=c.get_state()
    except Exception: time.sleep(1); continue
    if st.get("game_state_name")=="RACE" and st.get("race",{}).get("racers"): break
    time.sleep(1)
print("race reached after %.0fs; keys %s"%(time.time()-t0, list(st.get("race",{}).keys())[:12]))
def pspan(st):
    for r in st.get("race",{}).get("racers",[]):
        if r.get("is_player"): return r.get("span"), r.get("speed_raw")
    return None,None
time.sleep(2); print("hold throttle ->", c.command("hold_action",{"action":"throttle","slot":0,"frames":0}))
pending=sorted(TARGETS); t1=time.time(); last=None; shots=[]
while pending and time.time()-t1<420:
    try: st=c.get_state()
    except Exception: time.sleep(0.2); continue
    sp,spd=pspan(st)
    if sp is None: time.sleep(0.2); continue
    if sp!=last: last=sp
    if sp>=pending[0]:
        tgt=pending.pop(0); path=os.path.join(S,f"{os.environ.get('A1_PREFIX','a1')}_s{SEED}_span{tgt}_at{sp}.png")
        r=c.command("framedump",{"path":path}); time.sleep(0.6); shots.append((tgt,sp,path,os.path.exists(path) and os.path.getsize(path)))
        print(f"  span {sp} (target {tgt}) speed {spd} -> {os.path.basename(path)} ok={r.get('ok')}")
        if sp>pending[0] if pending else False: pass
    time.sleep(0.05)
print("done driving; span now",last,"; unreached targets",pending)
c.command("release_action",{"slot":0})
try: print("quit ->", c.command("quit")); proc.wait(30)
except Exception as e: print("quit/wait err",e); proc.kill()
log=open(os.path.join(WT,"log","race.log"),encoding="utf-8",errors="replace").read()
for m in re.findall(r".*(?:SUMMARY|summary:|on-road guard|r10cross|r8cross|r9city|flush_no_sidewalk|rejected).*", log)[:40]: print("LOG:", m.strip()[:230])
