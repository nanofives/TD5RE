#!/usr/bin/env python3
"""
MCP stdio-to-HTTP bridge for the x64dbg MCP plugin.

Codex/OpenAI talks MCP over stdio using Content-Length framed JSON-RPC
messages. The x64dbg plugin exposes an HTTP JSON-RPC endpoint. This bridge
translates between the two.

Env vars:
    X64DBG_MCP_HOST  default "127.0.0.1"
    X64DBG_MCP_PORT  default "3000"
"""

import json
import os
import sys
import urllib.error
import urllib.request

LOG_PATH = os.environ.get(
    "X64DBG_MCP_BRIDGE_LOG",
    "D:/Descargas/Test Drive 5 ISO/_organization/tools/x64dbg_bridge.log",
)


def log(message):
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as handle:
            handle.write(message + "\n")
    except Exception:
        pass


def make_error_response(req_id, code, message):
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "error": {"code": code, "message": message},
    }


def read_mcp_message():
    headers = {}

    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break

        try:
            key, value = line.decode("ascii").split(":", 1)
        except ValueError:
            continue
        headers[key.strip().lower()] = value.strip()

    content_length = headers.get("content-length")
    if content_length is None:
        return None

    body = sys.stdin.buffer.read(int(content_length))
    if not body:
        return None

    return json.loads(body.decode("utf-8"))


def write_mcp_message(message):
    raw = json.dumps(message, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(raw)
    sys.stdout.buffer.flush()


def post_json_rpc(base_url, request_obj):
    body = json.dumps(request_obj).encode("utf-8")
    req = urllib.request.Request(
        base_url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        resp_body = resp.read().decode("utf-8")

    if not resp_body.strip():
        return None

    return json.loads(resp_body)


def main():
    host = os.environ.get("X64DBG_MCP_HOST", "127.0.0.1")
    port = os.environ.get("X64DBG_MCP_PORT", "3000")
    base_url = f"http://{host}:{port}"
    log(f"bridge start base_url={base_url}")

    while True:
        try:
            request_obj = read_mcp_message()
            log(f"recv={json.dumps(request_obj, ensure_ascii=True) if request_obj is not None else 'EOF'}")
        except json.JSONDecodeError as exc:
            log(f"json decode error={exc}")
            write_mcp_message(make_error_response(None, -32700, f"Parse error: {exc}"))
            continue
        except Exception as exc:
            log(f"read exception={repr(exc)}")
            write_mcp_message(make_error_response(None, -32603, str(exc)))
            continue

        if request_obj is None:
            log("bridge exit on EOF")
            return

        req_id = request_obj.get("id")

        try:
            response_obj = post_json_rpc(base_url, request_obj)
            log(f"upstream response={json.dumps(response_obj, ensure_ascii=True) if response_obj is not None else 'NO_CONTENT'}")
        except urllib.error.URLError as exc:
            log(f"url error={repr(exc)}")
            response_obj = make_error_response(
                req_id,
                -32000,
                f"x64dbg MCP server unreachable at {base_url}: {exc}",
            )
        except Exception as exc:
            log(f"post exception={repr(exc)}")
            response_obj = make_error_response(req_id, -32603, str(exc))

        # JSON-RPC notifications do not expect a response on stdio.
        if req_id is None:
            log("skip stdio reply for notification")
            continue

        if response_obj is None:
            response_obj = make_error_response(
                req_id,
                -32603,
                "Upstream x64dbg MCP server returned an empty response to a request",
            )

        log(f"send={json.dumps(response_obj, ensure_ascii=True)}")
        write_mcp_message(response_obj)


if __name__ == "__main__":
    main()
