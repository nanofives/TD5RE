"""[R13 RAIL] Capture an IN-RACE frame via the live-control socket.

The env-var framedump alone is not reliable here: it fires on whatever is on
screen when the dump happens, and an autotrack level takes long enough to
generate that a fixed wait lands on the loading screen (observed at 130 s).
This polls get_state until it reports RACE and only then asks for a framedump,
which is the method the round's gotcha list prescribes.

  python r13rail_frame.py <port> <out.png> [timeout_s]
"""
import json, os, shutil, socket, sys, time

port = int(sys.argv[1])
out = sys.argv[2]
deadline = time.time() + (float(sys.argv[3]) if len(sys.argv) > 3 else 240.0)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)


def cmd(name, args=None):
    msg = json.dumps({"cmd": name, "args": args or {}}).encode()
    for _ in range(3):
        try:
            s.sendto(msg, ("127.0.0.1", port))
            return json.loads(s.recvfrom(65535)[0].decode())
        except Exception:
            continue
    return None


state = None
while time.time() < deadline:
    r = cmd("get_state")
    if r:
        state = json.dumps(r)
        if "RACE" in state.upper() and "PRERACE" not in state.upper():
            break
    time.sleep(2.0)
else:
    print("TIMEOUT waiting for RACE; last state: %s" % (state,))
    sys.exit(2)

print("in race: %s" % (state[:400],))
time.sleep(3.0)                       # let the countdown camera settle
rel = "log/r13rail_ctrl.png"
full = os.path.join(root, rel)
if os.path.exists(full):
    os.remove(full)
print("framedump ->", cmd("framedump", {"path": rel}))
for _ in range(40):
    if os.path.exists(full) and os.path.getsize(full) > 0:
        time.sleep(1.0)
        shutil.copyfile(full, out)
        print("saved %s (%d bytes)" % (out, os.path.getsize(out)))
        print("state at capture: %s" % (json.dumps(cmd("get_state"))[:400],))
        sys.exit(0)
    time.sleep(0.5)
print("NO FRAME produced")
sys.exit(3)
