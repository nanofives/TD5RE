#!/usr/bin/env python3
"""
Research all fill calls in CarSelectionScreenStateMachine via ghidra-headless-mcp TCP.
Uses correct tool names: project.program.open_existing, decomp.function, function.by_name, etc.
"""
import socket, json, sys, time

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
    # Check if it's an error result
    if result.get("isError"):
        content = result.get("content", [])
        texts = [c["text"] for c in content if c.get("type") == "text"]
        return "TOOL ERROR: " + "\n".join(texts)
    content = result.get("content", [])
    texts = [c["text"] for c in content if c.get("type") == "text"]
    return "\n".join(texts) if texts else json.dumps(result, indent=2)

def log(s, outf=None):
    print(s)
    if outf:
        outf.write(str(s) + "\n")
        outf.flush()

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills_final.txt"

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock, open(OUT_FILE, "w", encoding="utf-8") as outf:
    sock.settimeout(30)
    sock.connect((HOST, PORT))

    # Initialize session
    resp = tcp_call_raw(sock, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "carsel-research", "version": "1.0"}
    }, timeout=15)
    log(f"Init: {resp.get('result', {}).get('serverInfo', {})}", outf)

    # -------------------------------------------------------------------------
    # Step 1: Open the TD5 project
    # -------------------------------------------------------------------------
    log("\n=== OPENING PROGRAM ===", outf)
    r = tool_call(sock, "project.program.open_existing", {
        "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
        "project_name": "TD5",
        "program_name": "TD5_d3d.exe",
        "read_only": True
    }, timeout=60)
    log(r, outf)

    # Extract session_id from open response
    session_id = None
    for token in r.split():
        if token.startswith("session_id="):
            session_id = token.split("=", 1)[1]
            break
    log(f"Session ID: {session_id}", outf)

    def sc(tool_name, arguments=None, timeout=120):
        """Call tool with session_id injected."""
        args = dict(arguments or {})
        if session_id:
            args["session_id"] = session_id
        return tool_call(sock, tool_name, args, timeout=timeout)

    # -------------------------------------------------------------------------
    # Step 2: Find CarSelectionScreenStateMachine by name
    # -------------------------------------------------------------------------
    log("\n=== FINDING CarSelectionScreenStateMachine ===", outf)
    r = sc("function.by_name", {"name": "CarSelectionScreenStateMachine"})
    log(r, outf)

    # Also by address
    log("\n=== FUNCTION AT 0x40DFC0 ===", outf)
    r = sc("function.at", {"address": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 3: Decompile CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== DECOMP CarSelectionScreenStateMachine (full) ===", outf)
    r = sc("decomp.function", {"address": "0x40DFC0"}, timeout=180)
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 4: Search for fill/rect functions by name
    # -------------------------------------------------------------------------
    log("\n=== SEARCH: FillPrimaryFrontendRect ===", outf)
    r = sc("function.by_name", {"name": "FillPrimaryFrontendRect"})
    log(r, outf)

    log("\n=== SEARCH TEXT: Fill ===", outf)
    r = sc("search.text", {"query": "Fill"})
    log(r, outf)

    log("\n=== SEARCH TEXT: Copy16Bit ===", outf)
    r = sc("search.text", {"query": "Copy16Bit"})
    log(r, outf)

    log("\n=== SEARCH TEXT: CopyRect ===", outf)
    r = sc("search.text", {"query": "CopyRect"})
    log(r, outf)

    log("\n=== SEARCH TEXT: FillRect ===", outf)
    r = sc("search.text", {"query": "FillRect"})
    log(r, outf)

    log("\n=== SEARCH TEXT: blit ===", outf)
    r = sc("search.text", {"query": "blit"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 5: Get callees of CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== CALLEES of CarSelectionScreenStateMachine ===", outf)
    r = sc("function.callees", {"address": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 6: List all functions to find fill-like names
    # -------------------------------------------------------------------------
    log("\n=== FUNCTION LIST (all, filter for fill/rect/copy/blit) ===", outf)
    r = sc("function.list", {"offset": 0, "count": 1000})
    for line in r.split("\n"):
        ll = line.lower()
        if any(kw in ll for kw in ["fill", "rect", "copy", "blit", "clear", "surface", "frontend"]):
            log(line, outf)

    log("\n=== FUNCTION LIST page 2 ===", outf)
    r = sc("function.list", {"offset": 1000, "count": 1000})
    for line in r.split("\n"):
        ll = line.lower()
        if any(kw in ll for kw in ["fill", "rect", "copy", "blit", "clear", "surface", "frontend"]):
            log(line, outf)

    log(f"\n[Done - written to {OUT_FILE}]", outf)

print(f"\n[Written to {OUT_FILE}]")
