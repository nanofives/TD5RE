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

Gotchas learned 2026-09-04: hold_action expires after 60 frames unless frames=0; the
window is pushed to HWND_BOTTOM so the run never steals focus; the exe is the one at the
repo root above this script, so run it from the tree whose build you want to test.
"""
import os, sys, time, subprocess, ctypes, json, re
WT=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
S=sys.argv[1]; PORT=37072; TITLE="TD5RE-a1-37072"; SEED=sys.argv[2] if len(sys.argv)>2 else "99991"
START=int(sys.argv[3]) if len(sys.argv)>3 else 51
TARGETS=[int(x) for x in (sys.argv[4].split(',') if len(sys.argv)>4 else "66,75,95,115,145,150,160,311,358,361".split(','))]
sys.path.insert(0, os.path.join(WT,"scripts","td5re_mcp")); from game_client import GameClient
env=dict(os.environ, TD5RE_CONTROL_PORT=str(PORT), TD5RE_WINDOW_TITLE=TITLE, TD5RE_D3D12_CAPTURE="1",
         TD5RE_AUTOTRACK_SEED=SEED, TD5RE_AUTOTRACK_SCENERY="1", TD5RE_AUTOTRACK_BRIDGES="1", TD5RE_AUTOTRACK_PARKS="1")
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
