# TTD + WinDbg MCP workflow

Time Travel Debugging support for TD5RE. Use when standard Frida trace+hook
iteration is too slow (e.g. "where did this register value originate" — TTD
lets you step backward from the divergence point instead of recording many
forward runs).

## What's installed (2026-05-16)

| Component | Location | Notes |
|---|---|---|
| WinDbg + cdb + TTD | `C:\Program Files\WindowsApps\Microsoft.WinDbg_*\` (Store app) | Installed via `winget install Microsoft.WinDbg` |
| cdbX86.exe (32-bit replay) | `C:\Users\<user>\AppData\Local\Microsoft\WindowsApps\cdbX86.exe` | Symlink into the appx |
| TTD.exe (recorder) | `<appx>\amd64\ttd\TTD.exe` + `wow64\TTD.exe` | Bundled in WinDbg appx |
| pybag DLL cache | `tools\windbg-dlls\` (gitignored) | dbgeng.dll/dbghelp.dll/dbgmodel.dll copied here because WindowsApps perms block direct LoadLibrary |
| windbg-mcp server | `tools\windbg-mcp\` (gitignored, clone of gengstah/windbg-mcp) | 55 MCP tools wrapping pybag |
| Registration | `.mcp.json` at project root | Sets WINDBG_DIR env to the local DLL cache |

## Recording a trace

```powershell
# Record the original game (clean launch + record)
.\scripts\ttd_record.ps1 -Target original -Tag sydney-ai-1

# Record the port
.\scripts\ttd_record.ps1 -Target port -Tag honolulu-ai-baseline

# Attach to a running process
.\scripts\ttd_record.ps1 -ProcessId 12345 -Tag attached
```

The recorder spawns or attaches and runs TTD until the target process exits.
Trace lives at `traces\td5_<tag>_<timestamp>.run` (+ `.idx` auto-built on
first replay). Plan disk: 100-500 MB per minute of recorded execution.

**Performance note**: TTD adds ~10-20x slowdown. Don't record full races
unless necessary — record the minimum scenario that reproduces the bug, then
stop the target (ALT-F4 or `taskkill /F /IM <name>`).

## Replaying via the MCP

The `windbg` MCP exposes 55 tools (see `tools\windbg-mcp\windbg_mcp.py`
header for the full list). Typical TTD replay workflow:

1. `load_dump <path-to.run>` — open the trace
2. `raw "!tt 0"` — jump to recording start
3. `bp <function-address>` or `bp <module>!<symbol>` — set breakpoints
4. `go` — run forward to next breakpoint
5. `get_regs` / `read_mem <addr> <len>` — inspect state at the stop
6. `raw "g-"` — STEP BACKWARD one instruction (TTD-only command)
7. `raw "p-"` / `raw "t-"` — step over / step into backward
8. `raw "!tt <position>"` — jump to a specific TTD position
9. `raw "dx -g @$cursession.TTD.Events"` — list all timeline events
10. `disasm <addr>` — disassembly around address

The MCP's `raw` tool accepts any cdb command, so all TTD-specific
`!tt` extensions are available even though they're not first-class MCP tools.

## Workflow for our open Round 3 audits

### Wall response 0x00406980 (Sydney/BlueRidge invisible-wall shove)

```
1. .\scripts\ttd_record.ps1 -Target original -Tag sydney-wall
   (drive Sydney AI race via Frida quickrace until span 526 wall hit, then ALT-F4)

2. Via MCP:
   load_dump traces\td5_sydney-wall_*.run
   raw "bp 0x00406980"           # wall response entry
   raw "g"                       # run until first hit
   get_regs                      # capture v_para, delta, etc. at entry
   raw "g-"                      # step back to see how v_para was computed
   disasm 0x004068F0             # see the clamp-side decision

3. Repeat for port (td5re.exe equivalent function)
4. Compare branch decisions side-by-side
```

### Cascade saturation root

```
1. Record original Sydney race
2. Find the FIRST sub-tick where steering_command saturates to +/- 0x4000
3. Step backward from that write to see the input lateral_bias / target_offset
4. Compare to port's same sub-tick
```

### Recovery script state transitions

```
1. Record an extended race where AI loses control + spins
2. Use 'raw "dx -g @$cursession.TTD.Memory(<recovery_state_addr>, 4)"' for
   timeline of writes to the recovery-state field
3. Identify exit-condition checks via timeline of reads
```

## Caveats

- **TTD is read-only playback** — can't modify state inside the trace
- **No kernel mode** — user-mode only (fine for TD5_d3d.exe / td5re.exe)
- **Recording requires admin** — TTD needs elevated permissions to inject. Run PowerShell as Administrator.
- **Antivirus can interfere** — Windows Defender may flag TTD's injection. Whitelist `TTD.exe` if needed.
- **Trace files leak PII** — file paths, memory contents. Don't share traces without sanitizing.

## Why pybag needs local DLL copies

The WinDbg store-app install places `dbgeng.dll` / `dbghelp.dll` / `dbgmodel.dll` inside `C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\`, but WindowsApps directory permissions block `ctypes.LoadLibrary` even with read access. The `scripts\worktree_setup.ps1`-equivalent flow for this is:

```powershell
Copy-Item "C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\dbg*.dll" tools\windbg-dlls\
```

The `.mcp.json` sets `WINDBG_DIR=tools\windbg-dlls` so pybag uses the local copy on every MCP launch. No further config needed.

## Updating

When WinDbg updates (via winget or Store auto-update), re-run the DLL copy
step (the WindowsApps install path changes per version, but `WINDBG_DIR`
stays pointed at our local cache):

```powershell
$wdbg = Get-AppxPackage Microsoft.WinDbg | Select -First 1
$src  = Join-Path $wdbg.InstallLocation 'amd64'
foreach ($f in 'dbgeng.dll','dbghelp.dll','dbgmodel.dll','dbgcore.dll') {
    Copy-Item (Join-Path $src $f) tools\windbg-dlls\ -Force
}
```
