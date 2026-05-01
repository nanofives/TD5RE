#!/usr/bin/env python3
"""Run Ghidra MCP session: open project, decompile minimap functions."""
import subprocess, json, sys

GHIDRA_MCP = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra-headless-mcp/ghidra_headless_mcp.py"
GHIDRA_DIR = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_12.0.3_PUBLIC"

def run_session(messages, timeout=300):
    payload = "\n".join(json.dumps(m) for m in messages) + "\n"
    proc = subprocess.run(
        ["py", "-3.12", GHIDRA_MCP, "--ghidra-install-dir", GHIDRA_DIR],
        input=payload, capture_output=True, text=True, timeout=timeout
    )
    lines = [l.strip() for l in proc.stdout.splitlines() if l.strip()]
    results = {}
    for line in lines:
        try:
            obj = json.loads(line)
            if "id" in obj:
                results[obj["id"]] = obj
        except:
            pass
    return results, proc.stderr

def open_project():
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"claude","version":"1.0"}}},
        {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"project.program.open_existing","arguments":{
            "project_location":"C:/Users/maria/Desktop/Proyectos/TD5RE",
            "project_name":"TD5",
            "program_name":"TD5_d3d.exe",
            "read_only":True
        }}},
    ]
    results, stderr = run_session(msgs, timeout=120)
    r2 = results.get(2, {})
    if r2.get("error"):
        print("ERROR opening project:", r2["error"])
        print("STDERR:", stderr[-2000:])
        return None
    structured = r2.get("result",{}).get("structuredContent",{})
    content = r2.get("result",{}).get("content",[])
    for c in content:
        print("OPEN:", c.get("text","")[:300])
    print("Structured keys:", list(structured.keys()))
    sid = structured.get("session_id","")
    if not sid:
        # try content text
        for c in content:
            t = c.get("text","")
            if "session_id" in t:
                import re
                m = re.search(r'session_id["\s:]+([a-f0-9\-]+)', t)
                if m: sid = m.group(1)
    print(f"Session ID: {sid!r}")
    return sid

def decomp(sid, addr, name):
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"claude","version":"1.0"}}},
        {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"decomp.function","arguments":{
            "session_id":sid, "address":addr
        }}},
    ]
    results, stderr = run_session(msgs, timeout=180)
    res = results.get(2, {})
    if res.get("error"):
        print(f"ERROR decomp {name}:", res["error"])
        print("STDERR:", stderr[-500:])
        return
    print(f"\n{'='*70}")
    print(f"DECOMPILATION: {name} @ {addr}")
    print(f"{'='*70}")
    content = res.get("result",{}).get("content",[])
    for c in content:
        print(c.get("text",""))

if __name__ == "__main__":
    sid = open_project()
    if not sid:
        sys.exit(1)

    decomp(sid, "0x43B0A0", "InitMinimapLayout")
    decomp(sid, "0x43A220", "RenderTrackMinimapOverlay")
