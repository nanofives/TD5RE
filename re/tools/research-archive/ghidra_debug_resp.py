#!/usr/bin/env python3
"""Debug the response structure from ghidra-headless-mcp"""
import socket, json, time, re

HOST, PORT = "127.0.0.1", 8765
_msg_id = [100]

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

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(30)
    sock.connect((HOST, PORT))

    resp = tcp_call_raw(sock, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "debug", "version": "1.0"}
    }, timeout=15)

    # Open program
    mid = next_id()
    msg = {"jsonrpc": "2.0", "id": mid, "method": "tools/call", "params": {
        "name": "project.program.open_existing",
        "arguments": {
            "project_location": "C:/Users/maria/Desktop/Proyectos/TD5RE",
            "project_name": "TD5",
            "program_name": "TD5_d3d.exe",
            "read_only": True
        }
    }}
    sock.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    sock.settimeout(60)
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            line, _ = buf.split(b"\n", 1)
            try:
                obj = json.loads(line)
                if obj.get("id") == mid:
                    # Print FULL raw response
                    print("OPEN RESPONSE (raw):")
                    print(json.dumps(obj, indent=2)[:2000])
                    # Extract session_id
                    result = obj.get("result", {})
                    content = result.get("content", [])
                    for c in content:
                        if c.get("type") == "text":
                            text = c["text"]
                            m = re.search(r'session_id[=:](\S+)', text)
                            if m:
                                session_id = m.group(1).rstrip(',')
                                print(f"\nExtracted session_id: {session_id}")
                    break
            except Exception as e:
                print(f"Parse error: {e}")
                break

    # Now get the session_id from structured content
    result = obj.get("result", {})
    sc_content = result.get("structuredContent", {})
    print(f"\nStructured content: {json.dumps(sc_content, indent=2)[:1000]}")

    # Extract session_id
    session_id = None
    for c in result.get("content", []):
        if c.get("type") == "text":
            m = re.search(r'session_id[=\s]+(\w+)', c["text"])
            if m:
                session_id = m.group(1)

    if not session_id and sc_content:
        session_id = sc_content.get("session_id")

    print(f"\nFinal session_id: {session_id}")

    if session_id:
        # Try decomp.function with raw response dump
        mid2 = next_id()
        msg2 = {"jsonrpc": "2.0", "id": mid2, "method": "tools/call", "params": {
            "name": "decomp.function",
            "arguments": {"session_id": session_id, "function_start": "0x40DFC0"}
        }}
        sock.sendall((json.dumps(msg2) + "\n").encode())
        buf2 = b""
        sock.settimeout(180)
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf2 += chunk
            while b"\n" in buf2:
                line, buf2 = buf2.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    obj2 = json.loads(line)
                    if obj2.get("id") == mid2:
                        print("\n\nDECOMP RESPONSE (raw first 5000 chars):")
                        s = json.dumps(obj2, indent=2)
                        print(s[:5000])
                        break
                except Exception:
                    pass
            else:
                continue
            break
