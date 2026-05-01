#!/usr/bin/env python3
"""
Call ghidra-headless-mcp via existing TCP server on port 8765.
The server uses JSON-RPC 2.0 over TCP (newline-delimited).
"""
import socket, json, sys, time

HOST = "127.0.0.1"
PORT = 8765

def send_recv(sock, msg):
    data = json.dumps(msg) + "\n"
    sock.sendall(data.encode())
    # Read until we get a response with matching id
    buf = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
        # Try to parse each newline-delimited JSON
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                if obj.get("id") == msg.get("id"):
                    return obj
            except Exception:
                pass
        # If we have data and no more coming, try timeout
        sock.settimeout(2.0)
        try:
            more = sock.recv(65536)
            buf += more
        except socket.timeout:
            break
    return None

def call_tool(tool_name, arguments=None):
    arguments = arguments or {}
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(30.0)
        s.connect((HOST, PORT))

        # Initialize
        init = {
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "py-client", "version": "1.0"}
            }
        }
        resp = send_recv(s, init)
        if not resp:
            print("No init response", file=sys.stderr)
            return None

        # Call tool
        call = {
            "jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments}
        }
        s.settimeout(120.0)
        resp = send_recv(s, call)
        return resp

def extract_text(resp):
    if not resp:
        return "NO RESPONSE"
    if "error" in resp:
        return f"ERROR: {resp['error']}"
    result = resp.get("result", {})
    content = result.get("content", [])
    texts = []
    for c in content:
        if c.get("type") == "text":
            texts.append(c["text"])
    return "\n".join(texts) if texts else json.dumps(result, indent=2)

if __name__ == "__main__":
    tool = sys.argv[1] if len(sys.argv) > 1 else "health/ping"
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    resp = call_tool(tool, args)
    print(extract_text(resp))
