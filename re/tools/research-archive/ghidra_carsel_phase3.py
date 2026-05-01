#!/usr/bin/env python3
"""
Phase 3: Get exact CALL instruction addresses via listing,
get InitializeFrontendPresentationState,
and get PresentSecondaryFrontendRect/PresentPrimaryFrontendRect.
"""
import socket, json, time, re

HOST, PORT = "127.0.0.1", 8765
_msg_id = [400]

def next_id():
    _msg_id[0] += 1
    return _msg_id[0]

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_phase3.txt"
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
        return None, "NO RESPONSE"
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
    "clientInfo": {"name": "phase3", "version": "1.0"}
}, timeout=15)

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
# 1. Get code units at the CALL sites: 0x40ead7 and 0x40f2e7
#    These are the CALL FillPrimaryFrontendRect inside CarSelSM
# ============================================================
log("\n" + "="*70)
log("1. CODE UNIT AT 0x40ead7 (call site #1 in CarSelSM case 10)")
log("="*70)
sc2, txt2 = S("listing.code_unit.at", {"address": "0x40ead7"})
log(f"SC: {json.dumps(sc2, indent=2)}")
log(f"Txt: {txt2}")

# Get surrounding context: instructions before the CALL to see what was PUSH'd
for addr in ["0x40eac0", "0x40eac5", "0x40eacc", "0x40ead1", "0x40ead7"]:
    sc2, txt2 = S("listing.code_unit.at", {"address": addr})
    log(f"{addr}: SC={json.dumps(sc2, indent=2)[:200]} | {txt2[:100]}")

log("\n" + "="*70)
log("2. CODE UNIT AT 0x40f2e7 (call site #2 in CarSelSM case 0x14)")
log("="*70)
for addr in ["0x40f2d0", "0x40f2d5", "0x40f2dc", "0x40f2e2", "0x40f2e7"]:
    sc2, txt2 = S("listing.code_unit.at", {"address": addr})
    log(f"{addr}: SC={json.dumps(sc2, indent=2)[:200]} | {txt2[:100]}")

# ============================================================
# 3. Decompile InitializeFrontendPresentationState @ 0x424e40
#    (3rd caller at 0x424ea4)
# ============================================================
log("\n" + "="*70)
log("3. DECOMP InitializeFrontendPresentationState @ 0x424e40")
log("="*70)
sc2, txt2 = S("decomp.function", {"function_start": "0x424e40"}, timeout=120)
if sc2 and "c" in sc2:
    log(sc2["c"])
else:
    log(txt2[:1000])

# ============================================================
# 4. Decompile PresentSecondaryFrontendRect
# ============================================================
log("\n" + "="*70)
log("4. FIND+DECOMP PresentSecondaryFrontendRect")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "PresentSecondaryFrontendRect"})
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
# 5. Decompile PresentPrimaryFrontendRect
# ============================================================
log("\n" + "="*70)
log("5. FIND+DECOMP PresentPrimaryFrontendRect")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "PresentPrimaryFrontendRect"})
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
# 6. Decompile CopyPrimaryFrontendBufferToSecondary (state 3)
# ============================================================
log("\n" + "="*70)
log("6. FIND+DECOMP CopyPrimaryFrontendBufferToSecondary")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "CopyPrimaryFrontendBufferToSecondary"})
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
# 7. Decompile PresentPrimaryFrontendBufferViaCopy (state 4)
# ============================================================
log("\n" + "="*70)
log("7. FIND+DECOMP PresentPrimaryFrontendBufferViaCopy")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "PresentPrimaryFrontendBufferViaCopy"})
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
# 8. Get pcode at the fill call sites to confirm params
# ============================================================
log("\n" + "="*70)
log("8. PCODE at call sites")
log("="*70)
for addr in ["0x40ead7", "0x40f2e7"]:
    sc2, txt2 = S("pcode.op.at", {"address": addr})
    log(f"pcode at {addr}: {json.dumps(sc2, indent=2)[:500] if sc2 else txt2[:300]}")

# ============================================================
# 9. QueueFrontendOverlayRect - what is it?
# ============================================================
log("\n" + "="*70)
log("9. FIND+DECOMP QueueFrontendOverlayRect")
log("="*70)
sc2, txt2 = S("symbol.by_name", {"name": "QueueFrontendOverlayRect"})
if sc2 and sc2.get("items"):
    addr = sc2["items"][0]["address"]
    log(f"Address: {addr}")
    sc3, txt3 = S("decomp.function", {"function_start": addr})
    if sc3 and "c" in sc3:
        log(sc3["c"][:2000])
    else:
        log(txt3[:500])
else:
    log(f"Not found: {txt2[:200]}")

log("\n=== DONE ===")
outf.close()
sock.close()
print(f"\n[Written to {OUT_FILE}]")
