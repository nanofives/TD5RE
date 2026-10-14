"""r1030e_run.py -- round 1030 E runner: one La Plata (level091) race launch, then the numbers.

  python verify/r1030e_run.py --tag on  --rebuild            # item 9: fresh build, streamed scenery timeline
  python verify/r1030e_run.py --tag off --rebuild --extra TD5RE_AUTOTRACK_STREAM_ORDER=0
  python verify/r1030e_run.py --tag med --median 62,200     # item 10: drive slot 0 into the median at span 62
  python verify/r1030e_run.py --tag base --exe td5re_base.exe --rebuild   # master's exe, same build path

What it does (standing launch rules: windowed, sent to the back, RT off, minimum graphics, no sockets):
  * clears every TD5RE_* variable, then sets the place + the --extra knobs;
  * --rebuild deletes level091/GENSTAMP.TXT so the level is BUILT this launch (streamed path, the one
    Mariano gets on every new exe) instead of reused;
  * quits through an --InputScript `quit` (a clean shutdown, so race.log flushes) after --secs of race;
  * copies race.log / engine.log to log/r1030e_<tag>_*.log and prints the [R1030 E] / scenery stream /
    [MEDDRIVE] RESULT lines, then the level identity (md5 + sha256 + GENSTAMP).
Reads/writes only the worktree it lives in.
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(WT, 'log')
LVL = os.path.join(WT, 're', 'assets', 'levels', 'level091')

ap = argparse.ArgumentParser()
ap.add_argument('--tag', required=True)
ap.add_argument('--exe', default='td5re.exe')
ap.add_argument('--extra', nargs='*', default=[])
ap.add_argument('--rebuild', action='store_true')
ap.add_argument('--median', default='')          # "<span>,<speed>" -> TD5RE_MEDIAN_DRIVE
ap.add_argument('--gap', default='')             # "<run>" -> TD5RE_GAP_DRIVE (drive ACROSS a median gap)
ap.add_argument('--secs', type=int, default=95)  # race seconds before the scripted quit
ap.add_argument('--timeout', type=int, default=600)
ap.add_argument('--trace', action='store_true')  # [Trace] RaceTrace=1 for slot 0
ap.add_argument('--synthetic', action='store_true')  # level090, seed 99991, no place (move SELECTED.TXT aside first)
a = ap.parse_args()
if a.synthetic:
    LVL = os.path.join(WT, 're', 'assets', 'levels', 'level090')

env = {k: v for k, v in os.environ.items() if not k.upper().startswith('TD5RE_')}
if a.synthetic:
    env['TD5RE_AUTOTRACK_SEED'] = '99991'
else:
    env['TD5RE_GEO_PLACE'] = 'la_plata_partido'
env['TD5RE_RT'] = '0'
env['TD5RE_WINDOW_TITLE'] = 'TD5RE r1030e %s' % a.tag
if a.median:
    env['TD5RE_MEDIAN_DRIVE'] = a.median
if a.gap:
    env['TD5RE_GAP_DRIVE'] = a.gap
for kv in a.extra:
    k, v = kv.split('=', 1)
    env[k if k.startswith('TD5RE_') else 'TD5RE_' + k] = v

if a.rebuild:
    st = os.path.join(LVL, 'GENSTAMP.TXT')
    if os.path.exists(st):
        os.remove(st)
os.makedirs(LOG, exist_ok=True)
for f in ('race.log', 'engine.log', 'frontend.log'):
    p = os.path.join(LOG, f)
    for _ in range(20):
        try:
            if os.path.exists(p):
                os.remove(p)
            break
        except OSError:
            time.sleep(0.5)

script = os.path.join(LOG, 'r1030e_%s_input.txt' % a.tag)
with open(script, 'w') as fh:
    fh.write('sync race\n%d quit\n' % (a.secs * 60))

gfx = ['--Lighting=0', '--Quality=0', '--SunShadows=0', '--Reflections=0', '--WetRoads=0', '--StreetLights=0',
       '--CarLights=0', '--LegacyShadows=0', '--GIQuality=0', '--ShadowRays=0', '--ReflectionQuality=0',
       '--CarShadows=0', '--VFX=0', '--WorldBillboards=0', '--FoliageAA=0']
args = [os.path.join(WT, a.exe), '--AutoRace=1', '--SkipIntro=1',
        '--DefaultTrack=%d' % (60 if a.synthetic else 61), '--DefaultOpponents=0',
        '--Traffic=0', '--CarDamage=0', '--Windowed=1', '--VSync=0', '--Width=1280', '--Height=720',
        '--InputScript=%s' % script.replace('\\', '/')] + gfx
if a.median or a.gap:
    args += ['--PlayerIsAI=0', '--AutoThrottle=0']
else:
    args += ['--PlayerIsAI=1']
if a.trace:
    args += ['--RaceTrace=1', '--RaceTraceSlot=0', '--RaceTraceMaxFrames=40000']

t0 = time.time()
p = subprocess.Popen(args, cwd=WT, env=env)
print('pid=%d tag=%s exe=%s extra=%s median=%s gap=%s' % (p.pid, a.tag, a.exe, a.extra, a.median or '-', a.gap or '-'))

user32 = ctypes.windll.user32
sent = False
EnumWindows = user32.EnumWindows
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)


def windows_of(pid):
    found = []

    def cb(h, _):
        wp = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(wp))
        if wp.value == pid and user32.IsWindowVisible(ctypes.c_void_p(h)):
            found.append(h)
        return True
    EnumWindows(WNDENUMPROC(cb), 0)
    return found


while p.poll() is None and time.time() - t0 < a.timeout:
    time.sleep(0.5)
    if not sent:
        for h in windows_of(p.pid):
            user32.SetWindowPos(ctypes.c_void_p(h), ctypes.c_void_p(1), 0, 0, 0, 0, 0x0013)  # HWND_BOTTOM, no activate
            sent = True
killed = False
if p.poll() is None:
    for h in windows_of(p.pid):
        user32.PostMessageW(ctypes.c_void_p(h), 0x0010, 0, 0)   # WM_CLOSE: clean, flushes the logs
    for _ in range(40):
        if p.poll() is not None:
            break
        time.sleep(0.5)
    if p.poll() is None:
        p.kill()   # PID-scoped
        killed = True
print('game %s after %.0f s' % ('KILLED' if killed else 'exited (rc %s)' % p.returncode, time.time() - t0))

for f in ('race.log', 'engine.log'):
    src = os.path.join(LOG, f)
    if os.path.exists(src):
        shutil.copy(src, os.path.join(LOG, 'r1030e_%s_%s' % (a.tag, f)))
rl = os.path.join(LOG, 'r1030e_%s_race.log' % a.tag)
keys = ('[R1030 E]', 'scenery stream:', 'streamed scenery:', 'REUSED the on-disk', 'generated track ready',
        '[MEDDRIVE] RESULT', '[GAPDRIVE] RESULT', 'MEDIAN NOSE', 'median gap: run 0')
if os.path.exists(rl):
    for line in open(rl, encoding='utf-8', errors='replace'):
        if any(k in line for k in keys):
            print('  ' + line.rstrip()[:260])
else:
    print('  (no race.log)')
subprocess.call([sys.executable, '-I', os.path.join(WT, 'verify', 'geo_level_identity.py'), LVL], cwd=WT)
