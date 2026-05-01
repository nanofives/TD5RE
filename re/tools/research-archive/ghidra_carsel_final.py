#!/usr/bin/env python3
"""
Research all fill calls in CarSelectionScreenStateMachine via ghidra-headless-mcp TCP.
Correct argument names: function_start, text, etc.
"""
import socket, json, sys, time, re

HOST, PORT = "127.0.0.1", 8765
_msg_id = [10]

def next_id():
    _msg_id[0] += 1
    return _msg_id[0]

def tcp_call_raw(sock, method, params, timeout=120):
    mid = next_id()
    msg = {"jsonrpc": "2.0", "id": mid, "method": method, "params": params}
    sock.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    sock.settimeout(timeout)
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                if obj.get("id") == mid:
                    return obj
            except Exception:
                pass
    return None

def tool_call(sock, tool_name, arguments=None, timeout=120):
    resp = tcp_call_raw(sock, "tools/call", {"name": tool_name, "arguments": arguments or {}}, timeout=timeout)
    if not resp:
        return f"NO RESPONSE for {tool_name}"
    if "error" in resp:
        return f"RPC ERROR: {resp['error']}"
    result = resp.get("result", {})
    if result.get("isError"):
        content = result.get("content", [])
        texts = [c["text"] for c in content if c.get("type") == "text"]
        return "TOOL ERROR: " + "\n".join(texts)
    content = result.get("content", [])
    texts = [c["text"] for c in content if c.get("type") == "text"]
    return "\n".join(texts) if texts else json.dumps(result, indent=2)

def log(s, outf=None):
    print(str(s))
    if outf:
        outf.write(str(s) + "\n")
        outf.flush()

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills_final.txt"

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock, open(OUT_FILE, "w", encoding="utf-8") as outf:
    sock.settimeout(30)
    sock.connect((HOST, PORT))

    resp = tcp_call_raw(sock, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "carsel-research", "version": "1.0"}
    }, timeout=15)
    log(f"MCP server: {resp.get('result', {}).get('serverInfo', {})}", outf)

    # Open program
    log("\n=== OPEN PROGRAM ===", outf)
    r = tool_call(sock, "project.program.open_existing", {
        "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
        "project_name": "TD5",
        "program_name": "TD5_d3d.exe",
        "read_only": True
    }, timeout=60)
    log(r, outf)

    # Extract session_id
    session_id = None
    m = re.search(r'session_id=(\S+)', r)
    if m:
        session_id = m.group(1)
    log(f"Session: {session_id}", outf)

    def sc(tool_name, arguments=None, timeout=120):
        args = dict(arguments or {})
        if session_id:
            args["session_id"] = session_id
        return tool_call(sock, tool_name, args, timeout=timeout)

    # -------------------------------------------------------------------------
    # Find CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== function.by_name: CarSelectionScreenStateMachine ===", outf)
    r = sc("function.by_name", {"name": "CarSelectionScreenStateMachine"})
    log(r, outf)

    # Decompile it
    log("\n=== decomp.function @ 0x40DFC0 ===", outf)
    r = sc("decomp.function", {"function_start": "0x40DFC0"}, timeout=180)
    log(r, outf)

    # -------------------------------------------------------------------------
    # Find fill functions
    # -------------------------------------------------------------------------
    log("\n=== function.by_name: FillPrimaryFrontendRect ===", outf)
    r = sc("function.by_name", {"name": "FillPrimaryFrontendRect"})
    log(r, outf)

    # Search text for fill-related names
    for term in ["Fill", "fill", "Rect", "Copy16", "CopyRect", "Blit", "blit", "Clear"]:
        log(f"\n=== search.text: '{term}' ===", outf)
        r = sc("search.text", {"text": term})
        log(r[:2000] if len(r) > 2000 else r, outf)

    # -------------------------------------------------------------------------
    # Get callees
    # -------------------------------------------------------------------------
    log("\n=== function.callees @ 0x40DFC0 ===", outf)
    r = sc("function.callees", {"function_start": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Disassemble the function to find all CALL instructions
    # -------------------------------------------------------------------------
    log("\n=== listing.disassemble.function @ 0x40DFC0 ===", outf)
    r = sc("listing.disassemble.function", {"function_start": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # List all functions - look for fill/copy/rect
    # -------------------------------------------------------------------------
    log("\n=== function.list (filtering interesting names) ===", outf)
    for offset in range(0, 2000, 200):
        r = sc("function.list", {"offset": offset, "count": 200})
        found = False
        for line in r.split("\n"):
            ll = line.lower()
            if any(kw in ll for kw in ["fill", "rect", "copy", "blit", "clear", "surface", "frontend", "carsel"]):
                log(line, outf)
                found = True
        if not r.strip() or r.startswith("TOOL ERROR") or r.startswith("NO RESPONSE"):
            break

    # -------------------------------------------------------------------------
    # Decompile FillPrimaryFrontendRect if found
    # -------------------------------------------------------------------------
    log("\n=== function.by_name: Copy16BitSurfaceRect ===", outf)
    r = sc("function.by_name", {"name": "Copy16BitSurfaceRect"})
    log(r, outf)

    log("\n=== function.by_name: FillFrontendRect ===", outf)
    r = sc("function.by_name", {"name": "FillFrontendRect"})
    log(r, outf)

    log("\n=== function.by_name: BlitSurfaceRect ===", outf)
    r = sc("function.by_name", {"name": "BlitSurfaceRect"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # program.summary to understand the program
    # -------------------------------------------------------------------------
    log("\n=== program.summary ===", outf)
    r = sc("program.summary")
    log(r[:1000], outf)

    log(f"\n=== DONE ===", outf)

print(f"\n[Written to {OUT_FILE}]")
