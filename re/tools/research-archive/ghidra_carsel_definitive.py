#!/usr/bin/env python3
"""
DEFINITIVE: Research all fill calls in CarSelectionScreenStateMachine.
Reads structuredContent for actual decompilation data.
"""
import socket, json, time, re

HOST, PORT = "127.0.0.1", 8765
_msg_id = [200]

def next_id():
    _msg_id[0] += 1
    return _msg_id[0]

OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_carsel_fills_final.txt"
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

# Initialize
resp = tcp_send_recv(sock, "initialize", {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {"name": "definitive-research", "version": "1.0"}
}, timeout=15)
log(f"Server: {resp['result']['serverInfo']}")

# Open program
sc, txt = tool(sock, "project.program.open_existing", {
    "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
    "project_name": "TD5",
    "program_name": "TD5_d3d.exe",
    "read_only": True
}, timeout=60)
log(f"Open: {txt}")
sid = sc.get("session_id") if sc else None
# Also try extracting from text
if not sid:
    m = re.search(r'session_id[=\s]+(\w+)', txt)
    if m:
        sid = m.group(1)
log(f"session_id: {sid}")

def S(name, args, timeout=180):
    """Shorthand: call tool with session_id, return (sc, text)"""
    a = dict(args)
    a["session_id"] = sid
    return tool(sock, name, a, timeout=timeout)

# ============================================================
# 1. Decompile CarSelectionScreenStateMachine
# ============================================================
log("\n" + "="*70)
log("1. DECOMP CarSelectionScreenStateMachine @ 0x40DFC0")
log("="*70)
sc, txt = S("decomp.function", {"function_start": "0x40DFC0"}, timeout=180)
if sc and "c" in sc:
    decompC = sc["c"]
    log(decompC)
else:
    log(f"No C code in SC: {txt}")
    decompC = ""

# ============================================================
# 2. Get callees of CarSelectionScreenStateMachine
# ============================================================
log("\n" + "="*70)
log("2. CALLEES of CarSelectionScreenStateMachine @ 0x40DFC0")
log("="*70)
sc, txt = S("function.callees", {"function_start": "0x40DFC0"})
log(txt)
if sc:
    callees = sc.get("callees") or sc.get("functions") or []
    log(f"Structured callees: {json.dumps(callees, indent=2)[:3000]}")

# ============================================================
# 3. Search for fill/rect function names
# ============================================================
log("\n" + "="*70)
log("3. FUNCTION SEARCHES")
log("="*70)

for name in ["FillPrimaryFrontendRect", "Copy16BitSurfaceRect", "FillRect", "FillFrontendRect",
             "DrawFilledRect", "FillSurface", "ClearSurface", "RenderFill"]:
    sc2, txt2 = S("function.by_name", {"name": name})
    if sc2:
        fns = sc2.get("functions") or []
        if fns:
            log(f"  FOUND {name}: {json.dumps(fns, indent=2)}")
        else:
            log(f"  NOT FOUND: {name}")
    else:
        # count in text
        m = re.search(r'count=(\d+)', txt2)
        cnt = m.group(1) if m else "?"
        log(f"  {name}: count={cnt} | {txt2[:100]}")

# ============================================================
# 4. Search text for fill/rect in symbol names
# ============================================================
log("\n" + "="*70)
log("4. SYMBOL SEARCHES")
log("="*70)

for term in ["Fill", "Rect", "Clear", "Copy", "Blit"]:
    sc2, txt2 = S("search.text", {"text": term})
    if sc2:
        log(f"  search '{term}': {json.dumps(sc2, indent=2)[:500]}")
    else:
        log(f"  search '{term}': {txt2[:200]}")

# ============================================================
# 5. Decompile FillPrimaryFrontendRect if it exists
# ============================================================
log("\n" + "="*70)
log("5. DECOMPILE FILL FUNCTIONS")
log("="*70)

sc2, txt2 = S("function.by_name", {"name": "FillPrimaryFrontendRect"})
if sc2 and (sc2.get("functions") or []):
    fn_addr = sc2["functions"][0].get("address") or sc2["functions"][0].get("entry")
    log(f"FillPrimaryFrontendRect found at {fn_addr}")
    sc3, txt3 = S("decomp.function", {"function_start": fn_addr}, timeout=120)
    if sc3 and "c" in sc3:
        log(sc3["c"])
    else:
        log(txt3)
else:
    log("FillPrimaryFrontendRect: searching by symbol...")
    sc2, txt2 = S("symbol.by_name", {"name": "FillPrimaryFrontendRect"})
    log(f"symbol.by_name result: {json.dumps(sc2, indent=2)[:500] if sc2 else txt2[:300]}")

# ============================================================
# 6. Get all callers of FillPrimaryFrontendRect to find its address
# ============================================================
log("\n" + "="*70)
log("6. FUNCTION LIST - scan for fill/rect/copy names")
log("="*70)

# Get function list in chunks
for offset in [0, 200, 400, 600, 800]:
    sc2, txt2 = S("function.list", {"offset": offset, "count": 200})
    if sc2:
        fns = sc2.get("functions") or []
        for fn in fns:
            n = fn.get("name", "").lower()
            if any(kw in n for kw in ["fill", "rect", "copy", "blit", "clear", "surface", "frontend", "carsel", "render"]):
                log(f"  {fn.get('address') or fn.get('entry','?')} : {fn.get('name')}")
    else:
        # parse text
        for line in txt2.split("\n"):
            ll = line.lower()
            if any(kw in ll for kw in ["fill", "rect", "copy", "blit", "clear", "surface", "frontend"]):
                log(f"  {line}")

# ============================================================
# 7. Decompile Copy16BitSurfaceRect
# ============================================================
log("\n" + "="*70)
log("7. DECOMPILE Copy16BitSurfaceRect")
log("="*70)
sc2, txt2 = S("function.by_name", {"name": "Copy16BitSurfaceRect"})
if sc2 and (sc2.get("functions") or []):
    fn_addr = sc2["functions"][0].get("address") or sc2["functions"][0].get("entry")
    log(f"Copy16BitSurfaceRect at {fn_addr}")
    sc3, txt3 = S("decomp.function", {"function_start": fn_addr}, timeout=120)
    if sc3 and "c" in sc3:
        log(sc3["c"])
    else:
        log(txt3)
else:
    log(f"Copy16BitSurfaceRect: {txt2[:200]}")

log("\n" + "="*70)
log("DONE")
log("="*70)

outf.close()
sock.close()
print(f"\n[Written to {OUT_FILE}]")
