#!/usr/bin/env python3
"""
Research all fill calls in CarSelectionScreenStateMachine via ghidra-headless-mcp TCP.
"""
import socket, json, sys, time

HOST, PORT = "127.0.0.1", 8765
_msg_id = [10]

def next_id():
    _msg_id[0] += 1
    return _msg_id[0]

def tcp_call(sock, method, params):
    mid = next_id()
    msg = {"jsonrpc": "2.0", "id": mid, "method": method, "params": params}
    sock.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    sock.settimeout(120.0)
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

def tool_call(sock, tool_name, arguments=None):
    resp = tcp_call(sock, "tools/call", {"name": tool_name, "arguments": arguments or {}})
    if not resp:
        return f"NO RESPONSE for {tool_name}"
    if "error" in resp:
        return f"RPC ERROR: {resp['error']}"
    result = resp.get("result", {})
    content = result.get("content", [])
    texts = [c["text"] for c in content if c.get("type") == "text"]
    return "\n".join(texts) if texts else json.dumps(result, indent=2)

def log(s, outf=None):
    print(s)
    if outf:
        outf.write(str(s) + "\n")
        outf.flush()

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills_final.txt"

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s, open(OUT_FILE, "w", encoding="utf-8") as outf:
    s.settimeout(30)
    s.connect((HOST, PORT))

    # Initialize
    resp = tcp_call(s, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "carsel-research", "version": "1.0"}
    })
    log(f"Init: {resp.get('result', {}).get('serverInfo', {})}", outf)

    # -------------------------------------------------------------------------
    # Step 1: Open the program
    # -------------------------------------------------------------------------
    log("\n=== OPENING PROGRAM ===", outf)
    r = tool_call(s, "project_program_open_existing", {
        "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
        "project_name": "TD5",
        "program_name": "TD5_d3d.exe",
        "read_only": True
    })
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 2: Find CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== FINDING CarSelectionScreenStateMachine ===", outf)
    r = tool_call(s, "function_by_name", {"name": "CarSelectionScreenStateMachine"})
    log(r, outf)

    # Also try by address
    log("\n=== FUNCTION AT 0x40DFC0 ===", outf)
    r = tool_call(s, "function_at", {"address": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 3: Decompile CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== DECOMP CarSelectionScreenStateMachine ===", outf)
    r = tool_call(s, "decomp_function", {"address": "0x40DFC0"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 4: Search for fill/rect functions
    # -------------------------------------------------------------------------
    log("\n=== SEARCH: FillPrimaryFrontendRect ===", outf)
    r = tool_call(s, "function_by_name", {"name": "FillPrimaryFrontendRect"})
    log(r, outf)

    log("\n=== SEARCH TEXT: Fill ===", outf)
    r = tool_call(s, "search_text", {"query": "Fill", "search_type": "functions"})
    log(r, outf)

    log("\n=== SEARCH TEXT: fill ===", outf)
    r = tool_call(s, "search_text", {"query": "fill", "search_type": "functions"})
    log(r, outf)

    log("\n=== SEARCH TEXT: Copy16Bit ===", outf)
    r = tool_call(s, "search_text", {"query": "Copy16Bit", "search_type": "functions"})
    log(r, outf)

    log("\n=== SEARCH TEXT: CopyRect ===", outf)
    r = tool_call(s, "search_text", {"query": "CopyRect", "search_type": "functions"})
    log(r, outf)

    # -------------------------------------------------------------------------
    # Step 5: Get callees of CarSelectionScreenStateMachine
    # -------------------------------------------------------------------------
    log("\n=== CALLEES of CarSelectionScreenStateMachine ===", outf)
    r = tool_call(s, "function_callees", {"address": "0x40DFC0"})
    log(r, outf)

    log(f"\n[Done - written to {OUT_FILE}]", outf)

print(f"\n[Written to {OUT_FILE}]")
