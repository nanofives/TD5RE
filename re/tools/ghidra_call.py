#!/usr/bin/env python3
"""Wrapper to call ghidra-headless-mcp tools via stdio JSON-RPC.

Usage:
    python ghidra_call.py <tool_name> [json_arguments]
    python ghidra_call.py health.ping
    python ghidra_call.py program.open '{"file_path":"/path/to/binary"}'
    python ghidra_call.py program.open '{"file_path":"/path/to/binary"}' --raw
"""
import subprocess, sys, json

GHIDRA_MCP = r"C:\Users\maria\AppData\Local\Packages\PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0\LocalCache\local-packages\Python313\Scripts\ghidra-headless-mcp.EXE"
GHIDRA_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"

def call_tool(tool_name, arguments=None, raw=False):
    arguments = arguments or {}
    init_msg = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "claude-cli", "version": "1.0"}
        }
    })
    call_msg = json.dumps({
        "jsonrpc": "2.0", "id": 2, "method": "tools/call",
        "params": {"name": tool_name, "arguments": arguments}
    })
    stdin_data = init_msg + "\n" + call_msg + "\n"

    proc = subprocess.run(
        [GHIDRA_MCP, "--ghidra-install-dir", GHIDRA_DIR],
        input=stdin_data, capture_output=True, text=True, timeout=120
    )

    lines = proc.stdout.strip().split("\n")
    for line in lines:
        try:
            msg = json.loads(line)
            if msg.get("id") == 2:
                if raw:
                    print(json.dumps(msg, indent=2))
                elif "result" in msg:
                    result = msg["result"]
                    # Print structured content if available, else text
                    if "structuredContent" in result:
                        print(json.dumps(result["structuredContent"], indent=2))
                    elif "content" in result:
                        for c in result["content"]:
                            print(c.get("text", json.dumps(c)))
                    else:
                        print(json.dumps(result, indent=2))
                elif "error" in msg:
                    print(f"ERROR: {msg['error']['message']}", file=sys.stderr)
                    sys.exit(1)
                return
        except json.JSONDecodeError:
            continue

    print("No response received", file=sys.stderr)
    if proc.stderr:
        print(proc.stderr[:500], file=sys.stderr)
    sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python ghidra_call.py <tool_name> [json_arguments] [--raw]")
        sys.exit(1)

    tool = sys.argv[1]
    raw = "--raw" in sys.argv
    args_str = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "--raw" else "{}"

    try:
        args = json.loads(args_str)
    except json.JSONDecodeError:
        print(f"Invalid JSON arguments: {args_str}", file=sys.stderr)
        sys.exit(1)

    call_tool(tool, args, raw)
