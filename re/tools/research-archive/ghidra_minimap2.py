#!/usr/bin/env python3
"""Open TD5_headless and decompile minimap functions."""
import subprocess, json, sys, re

GHIDRA_MCP = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra-headless-mcp/ghidra_headless_mcp.py"
GHIDRA_DIR = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_12.0.3_PUBLIC"
PY = sys.executable  # use current interpreter (py -3.12)

def mcp_call(messages, timeout=180):
    payload = "\n".join(json.dumps(m) for m in messages) + "\n"
    proc = subprocess.run([PY, GHIDRA_MCP, "--ghidra-install-dir", GHIDRA_DIR],
        input=payload, capture_output=True, text=True, timeout=timeout)
    results = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line:
            try:
                d = json.loads(line)
                if "id" in d:
                    results[d["id"]] = d
            except: pass
    return results, proc.stderr

# --- Step 1: Open project ---
msgs_open = [
    {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}},
    {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"project.program.open_existing","arguments":{
        "project_location":"C:/Users/maria/Desktop/Proyectos/TD5RE",
        "project_name":"TD5_headless",
        "program_name":"TD5_d3d.exe",
        "read_only":True
    }}},
]
results, stderr = mcp_call(msgs_open, timeout=120)
r = results.get(2, {})
if r.get("error"):
    print("OPEN ERROR:", r["error"])
    print("STDERR:", stderr[-1000:])
    sys.exit(1)
sc = r.get("result", {}).get("structuredContent", {})
content = r.get("result", {}).get("content", [])
sid = sc.get("session_id", "")
for c in content:
    print("OPEN:", c.get("text", ""))
print(f"Session: {sid}")
if not sid:
    sys.exit(1)

# --- Step 2: Decompile functions ---
FUNCS = [
    ("0x43B0A0", "InitMinimapLayout"),
    ("0x43A220", "RenderTrackMinimapOverlay"),
]

for addr, name in FUNCS:
    print(f"\n{'='*70}")
    print(f"FUNCTION: {name} @ {addr}")
    print(f"{'='*70}")
    msgs_decomp = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}},
        {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"decomp.function","arguments":{
            "session_id": sid, "function_start": addr, "timeout_secs": 120
        }}},
    ]
    results2, stderr2 = mcp_call(msgs_decomp, timeout=180)
    r2 = results2.get(2, {})
    if r2.get("error"):
        print("DECOMP ERROR:", r2["error"])
        print("STDERR:", stderr2[-500:])
        continue
    content2 = r2.get("result", {}).get("content", [])
    for c in content2:
        print(c.get("text", ""))
