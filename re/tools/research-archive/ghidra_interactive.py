#!/usr/bin/env python3
"""Interactive Ghidra MCP session: open project + decompile functions."""
import subprocess, json, sys, threading, queue

GHIDRA_MCP = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra-headless-mcp/ghidra_headless_mcp.py"
GHIDRA_DIR = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_12.0.3_PUBLIC"

proc = subprocess.Popen(
    [sys.executable, GHIDRA_MCP, "--ghidra-install-dir", GHIDRA_DIR],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, bufsize=1
)

def send(msg):
    line = json.dumps(msg) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()

def recv():
    while True:
        line = proc.stdout.readline()
        if not line:
            break
        line = line.strip()
        if line:
            try:
                return json.loads(line)
            except:
                pass
    return None

def call(msg_id, method, params):
    send({"jsonrpc":"2.0","id":msg_id,"method":method,"params":params})
    while True:
        r = recv()
        if r and r.get("id") == msg_id:
            return r

# Initialize
r = call(1, "initialize", {"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}})
print("Init:", r.get("result",{}).get("serverInfo",{}).get("name","?"))

# Open project
r = call(2, "tools/call", {"name":"project.program.open_existing","arguments":{
    "project_location":"C:/Users/maria/Desktop/Proyectos/TD5RE",
    "project_name":"TD5_headless",
    "program_name":"TD5_d3d.exe",
    "read_only":True
}})
if r.get("error"):
    print("OPEN ERROR:", r["error"])
    proc.terminate()
    sys.exit(1)
sc = r.get("result",{}).get("structuredContent",{})
sid = sc.get("session_id","")
print(f"Session: {sid}")

# Decompile functions
FUNCS = [
    ("0x43B0A0", "InitMinimapLayout"),
    ("0x43A220", "RenderTrackMinimapOverlay"),
]
for i, (addr, name) in enumerate(FUNCS, start=3):
    print(f"\n{'='*70}")
    print(f"FUNCTION: {name} @ {addr}")
    print(f"{'='*70}")
    r2 = call(i, "tools/call", {"name":"decomp.function","arguments":{
        "session_id":sid, "function_start":addr, "timeout_secs":120
    }})
    if r2.get("error"):
        print("ERROR:", r2["error"])
        continue
    for c in r2.get("result",{}).get("content",[]):
        print(c.get("text",""))

proc.stdin.close()
proc.wait(timeout=10)
