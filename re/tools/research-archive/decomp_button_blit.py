"""
Find and decompile functions related to blitting buttons to screen.
Look for: DrawFrontendButtons, RenderFrontendButtons, or any function referencing DAT_00499c9c (button table).
"""
import pyghidra
import sys, os, traceback

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5_copy_tmp"
COPY_PROJ_NAME = "TD5_copy"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_button_blit.txt"

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.address import AddressSet

results = []

try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)

    decomp = DecompInterface()
    opts = DecompileOptions()
    decomp.setOptions(opts)
    decomp.openProgram(prog)
    monitor = ConsoleTaskMonitor()

    fm = prog.getFunctionManager()
    ref_mgr = prog.getReferenceManager()
    sym_tbl = prog.getSymbolTable()
    addr_factory = prog.getAddressFactory()

    # Key addresses to look for
    target_addrs = [
        # DAT_00499c9c - button surface pointer array
        "0x00499c9c",
        # DAT_0049a978 - button active flag
        "0x0049a978",
    ]

    # Find functions that reference the button table address (DAT_00499c9c)
    btn_table_addr = addr_factory.getAddress("0x00499c9c")
    refs = list(ref_mgr.getReferencesTo(btn_table_addr))

    blit_candidates = set()
    for ref in refs:
        caller_addr = ref.getFromAddress()
        caller_fn = fm.getFunctionContaining(caller_addr)
        if caller_fn:
            blit_candidates.add(caller_fn)

    results.append(f"Functions referencing button table (0x00499c9c): {len(blit_candidates)}")
    for f in sorted(blit_candidates, key=lambda fn: fn.getEntryPoint().getOffset()):
        results.append(f"  {f.getEntryPoint()} {f.getName()}")

    # Also search by name patterns
    name_patterns = ["DrawButton", "RenderButton", "BlitButton", "PresentButton",
                     "DrawFrontend", "RenderFrontend", "BlitFrontend",
                     "PaintFrontend", "FlushButton", "UpdateButton"]

    named_fns = set()
    for fn in fm.getFunctions(True):
        name = fn.getName()
        for pat in name_patterns:
            if pat.lower() in name.lower():
                named_fns.add(fn)
                break

    results.append(f"\nFunctions matching name patterns: {len(named_fns)}")
    for f in sorted(named_fns, key=lambda fn: fn.getEntryPoint().getOffset()):
        results.append(f"  {f.getEntryPoint()} {f.getName()}")

    # Decompile the most relevant ones (button table refs + named)
    to_decompile = blit_candidates | named_fns

    already_done = set()
    for fn in sorted(to_decompile, key=lambda f: f.getEntryPoint().getOffset()):
        key = str(fn.getEntryPoint())
        if key in already_done:
            continue
        already_done.add(key)
        results.append(f"\n{'='*60}")
        results.append(f"FUNCTION: {fn.getEntryPoint()} {fn.getName()}")
        results.append('='*60)
        res = decomp.decompileFunction(fn, 90, monitor)
        if res.decompileCompleted():
            results.append(res.getDecompiledFunction().getC())
        else:
            results.append(f"FAILED: {res.getErrorMessage()}")

    decomp.dispose()
    proj.close(prog)
    proj.close()

except Exception as e:
    results.append(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
# Print summary only
print(output[:4000])
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)
print(f"\nFull output written to {OUT_FILE}")
