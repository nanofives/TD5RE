#!/bin/bash
# Wrapper to call ghidra-headless-mcp tools via stdio
# Usage: bash ghidra_call.sh <tool_name> '<json_arguments>'
# Example: bash ghidra_call.sh health.ping '{}'
# Example: bash ghidra_call.sh program.open '{"file_path":"/path/to/binary"}'

GHIDRA_MCP="C:/Users/maria/AppData/Local/Packages/PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0/LocalCache/local-packages/Python313/Scripts/ghidra-headless-mcp.EXE"
GHIDRA_DIR="C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_12.0.3_PUBLIC"

TOOL_NAME="$1"
TOOL_ARGS="${2:-{}}"

{
  echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"claude-cli","version":"1.0"}}}'
  echo "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"$TOOL_NAME\",\"arguments\":$TOOL_ARGS}}"
} | "$GHIDRA_MCP" --ghidra-install-dir "$GHIDRA_DIR" 2>/dev/null | tail -1
