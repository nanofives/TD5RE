#!/usr/bin/env python3
"""
Phase 2: Decompile FillPrimaryFrontendRect, DrawCarSelectionPreviewOverlay,
and get exact CALL instruction addresses via disassembly listing.
"""
import socket, json, time, re

HOST, PORT = "127.0.0.1", 8765
_msg_id = [300]

def next_id():
    _msg_id[0] += 1
    return _msg_id[0]

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_phase2.txt"
outf = open(OUT_FILE, "w", encoding="utf-8")

def log(s):
    print(str(s))
    outf.write(str(s) + "\n")
    outf.flush()

def tcp_send_recv(sock, method, params, timeout=180):
    mid = next_id()
    msg = {"jsonrpc": "2.0", "id": mid, "method": method, "params": params}
    sock.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    sock.settimeout(timeout)
    while True:
        try:
            chunk = sock.recv(131072)
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

def tool(sock, name, args, timeout=180):
    resp = tcp_send_recv(sock, "tools/call", {"name": name, "arguments": args}, timeout=timeout)
    if not resp:
        return None, f"NO RESPONSE"
    if "error" in resp:
        return None, f"RPC ERROR: {resp['error']}"
    result = resp.get("result", {})
    sc = result.get("structuredContent") or {}
    text_parts = [c["text"] for c in result.get("content", []) if c.get("type") == "text"]
    text = "\n".join(text_parts)
    return sc, text

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(30)
sock.connect((HOST, PORT))

resp = tcp_send_recv(sock, "initialize", {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {"name": "phase2", "version": "1.0"}
}, timeout=15)
log(f"Server: {resp['result']['serverInfo']}")

# Open program
sc, txt = tool(sock, "project.program.open_existing", {
    "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
    "project_name": "TD5",
    "program_name": "TD5_d3d.exe",
    "read_only": True
}, timeout=60)
sid = sc.get("session_id") if sc else None
if not sid:
    m = re.search(r'session_id[=\s]+(\w+)', txt)
    if m:
        sid = m.group(1)
log(f"session_id: {sid}")

def S(name, args, timeout=180):
    a = dict(args)
    a["session_id"] = sid
    return tool(sock, name, a, timeout=timeout)

# ============================================================
# 1. Decompile FillPrimaryFrontendRect @ 0x423ed0
# ============================================================
log("\n" + "="*70)
log("1. DECOMP FillPrimaryFrontendRect @ 0x423ed0")
log("="*70)
sc2, txt2 = S("decomp.function", {"function_start": "0x423ed0"}, timeout=120)
if sc2 and "c" in sc2:
    log(sc2["c"])
else:
    log(f"SC: {json.dumps(sc2, indent=2)[:500]}")
    log(f"Txt: {txt2}")

# ============================================================
# 2. Find DrawCarSelectionPreviewOverlay
# ============================================================
log("\n" + "="*70)
log("2. FIND DrawCarSelectionPreviewOverlay")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "DrawCarSelectionPreviewOverlay"})
log(f"SC: {json.dumps(sc2, indent=2)[:500]}")

# Get its address from symbol result
draw_addr = None
if sc2 and sc2.get("items"):
    draw_addr = sc2["items"][0].get("address")
    log(f"Address: {draw_addr}")

if draw_addr:
    log("\n--- Decompile DrawCarSelectionPreviewOverlay ---")
    sc3, txt3 = S("decomp.function", {"function_start": draw_addr}, timeout=180)
    if sc3 and "c" in sc3:
        log(sc3["c"])
    else:
        log(txt3[:1000])

# ============================================================
# 3. Get disassembly listing for CarSelectionScreenStateMachine
#    to find exact CALL addresses for FillPrimaryFrontendRect
# ============================================================
log("\n" + "="*70)
log("3. LISTING CODE UNITS for FillPrimaryFrontendRect CALLS")
log("="*70)

# FillPrimaryFrontendRect is at 0x423ed0
# Find all references TO it (callers in CarSelSM)
sc2, txt2 = S("reference.to", {"address": "0x423ed0"})
log(f"References TO FillPrimaryFrontendRect (0x423ed0):")
if sc2:
    items = sc2.get("items") or sc2.get("references") or []
    log(f"SC: {json.dumps(sc2, indent=2)[:3000]}")
else:
    log(f"Txt: {txt2[:1000]}")

# ============================================================
# 4. Get callers of FillPrimaryFrontendRect
# ============================================================
log("\n" + "="*70)
log("4. CALLERS of FillPrimaryFrontendRect @ 0x423ed0")
log("="*70)
sc2, txt2 = S("function.callers", {"function_start": "0x423ed0"})
if sc2:
    log(f"SC: {json.dumps(sc2, indent=2)[:2000]}")
else:
    log(f"Txt: {txt2[:1000]}")

# ============================================================
# 5. Get listing code units at known CALL sites
#    From the decompile we know:
#    - case 10: FillPrimaryFrontendRect(0x5c,uVar1,iVar16,0x198,300)
#    - case 0x14: FillPrimaryFrontendRect(0x5c,uVar1,iVar16,0x198,300)
#    Let's get the raw listing around those areas
# ============================================================
log("\n" + "="*70)
log("5. PCODE for CarSelectionScreenStateMachine (find fill calls)")
log("="*70)
# Search for references to 0x423ed0 within the function body
sc2, txt2 = S("search.constants", {"value": "0x423ed0"})
log(f"search.constants 0x423ed0: {json.dumps(sc2, indent=2)[:1000] if sc2 else txt2[:500]}")

# ============================================================
# 6. Decompile specific states by looking at code units
# ============================================================
log("\n" + "="*70)
log("6. LISTING CODE UNITS around CarSelSM (case 10 area)")
log("="*70)
# Case 10 starts with FillPrimaryFrontendRect - let's get listing
# The function is at 0x40DFC0. Let's try listing a range.
sc2, txt2 = S("listing.code_units.list", {
    "start_address": "0x40e990",  # approximate area of case 10
    "end_address": "0x40ea50",
    "include_instructions": True
})
if sc2:
    log(f"SC: {json.dumps(sc2, indent=2)[:5000]}")
else:
    log(f"Txt: {txt2[:2000]}")

# ============================================================
# 7. Search instructions for CALL to 0x423ed0
# ============================================================
log("\n" + "="*70)
log("7. SEARCH INSTRUCTIONS: call to FillPrimaryFrontendRect")
log("="*70)
sc2, txt2 = S("search.instructions", {"pattern": "CALL 0x423ed0"})
if sc2:
    log(f"SC: {json.dumps(sc2, indent=2)[:3000]}")
else:
    log(f"Txt: {txt2[:1000]}")

# Also search pcode
sc2, txt2 = S("search.pcode", {"pattern": "CALL 0x423ed0"})
if sc2:
    log(f"pcode search: {json.dumps(sc2, indent=2)[:1000]}")
else:
    log(f"pcode search txt: {txt2[:500]}")

# ============================================================
# 8. Decompile CopyPrimaryFrontendRectToSecondary
# ============================================================
log("\n" + "="*70)
log("8. FIND CopyPrimaryFrontendRectToSecondary")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "CopyPrimaryFrontendRectToSecondary"})
if sc2 and sc2.get("items"):
    addr = sc2["items"][0]["address"]
    log(f"Address: {addr}")
    sc3, txt3 = S("decomp.function", {"function_start": addr})
    if sc3 and "c" in sc3:
        log(sc3["c"])
    else:
        log(txt3[:1000])
else:
    log(f"Not found: {txt2[:200]}")

# ============================================================
# 9. Decompile PresentPrimaryFrontendBuffer
# ============================================================
log("\n" + "="*70)
log("9. FIND PresentPrimaryFrontendBuffer")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "PresentPrimaryFrontendBuffer"})
if sc2 and sc2.get("items"):
    addr = sc2["items"][0]["address"]
    log(f"Address: {addr}")
    sc3, txt3 = S("decomp.function", {"function_start": addr})
    if sc3 and "c" in sc3:
        log(sc3["c"])
    else:
        log(txt3[:500])
else:
    log(f"Not found: {txt2[:200]}")

log("\n=== DONE ===")
outf.close()
sock.close()
print(f"\n[Written to {OUT_FILE}]")
