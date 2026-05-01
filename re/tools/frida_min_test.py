import os, subprocess, sys, time, frida
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GAME_DIR = os.path.join(REPO, "original")
GAME_EXE = os.path.join(GAME_DIR, "TD5_d3d.exe")
HOOK = os.path.join(HERE, "frida_main_menu_hook_min.js")

env = os.environ.copy()
env["__COMPAT_LAYER"] = "RunAsInvoker"
proc = subprocess.Popen([GAME_EXE], cwd=GAME_DIR, env=env)
print(f"pid={proc.pid}")

t0 = time.time()
session = None
while time.time() - t0 < 10:
    try:
        session = frida.attach(proc.pid)
        break
    except Exception as e:
        time.sleep(0.05)
if session is None:
    print("attach failed")
    proc.kill(); sys.exit(1)
print("attached")

with open(HOOK) as f: src = f.read()
script = session.create_script(src)
def on_message(msg, _):
    print("MSG", msg)
script.on("message", on_message)
script.load()
print("loaded — sleeping 8s")
time.sleep(8)
try: session.detach()
except: pass
try: proc.terminate(); proc.wait(2)
except:
    try: proc.kill()
    except: pass
