"""
Extended analysis: callers of FUN_00424560 and FUN_00425b60, plus decompile callers.
"""
import pyghidra
import sys, os, traceback, shutil

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
COPY_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\TD5_copy_tmp"
COPY_PROJ_NAME = "TD5_copy"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_buttons_callers.txt"

TARGETS = ["0x00424560", "0x00425b60"]

pyghidra.start(install_dir=GHIDRA_INSTALL)

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import FlowType

results = []

try:
    proj = GhidraProject.openProject(COPY_DIR, COPY_PROJ_NAME, True)
    prog = proj.openProgram("/", "TD5_d3d.exe", True)
    print(f"Opened: {prog.getName()}, {prog.getFunctionManager().getFunctionCount()} functions")

    decomp = DecompInterface()
    opts = DecompileOptions()
    decomp.setOptions(opts)
    decomp.openProgram(prog)
    monitor = ConsoleTaskMonitor()

    fm = prog.getFunctionManager()
    ref_mgr = prog.getReferenceManager()
    addr_factory = prog.getAddressFactory()

    # For each target, find callers and decompile them
    already_decompiled = set()

    for addrStr in TARGETS:
        addr = addr_factory.getAddress(addrStr)
        fn = fm.getFunctionAt(addr)
        results.append(f"\n{'='*60}")
        results.append(f"TARGET: {addrStr} = {fn.getName() if fn else 'NOT FOUND'}")
        results.append('='*60)

        # Find callers via references
        refs = list(ref_mgr.getReferencesTo(addr))
        caller_fns = set()
        for ref in refs:
            caller_addr = ref.getFromAddress()
            caller_fn = fm.getFunctionContaining(caller_addr)
            if caller_fn:
                caller_fns.add(caller_fn)

        results.append(f"\nCallers ({len(caller_fns)}):")
        for cf in sorted(caller_fns, key=lambda f: f.getEntryPoint().getOffset()):
            results.append(f"  {cf.getEntryPoint()} {cf.getName()}")

        # Decompile each unique caller
        for cf in sorted(caller_fns, key=lambda f: f.getEntryPoint().getOffset()):
            key = str(cf.getEntryPoint())
            if key in already_decompiled:
                continue
            already_decompiled.add(key)
            results.append(f"\n--- CALLER: {cf.getEntryPoint()} {cf.getName()} ---")
            res = decomp.decompileFunction(cf, 90, monitor)
            if res.decompileCompleted():
                results.append(res.getDecompiledFunction().getC())
            else:
                results.append(f"DECOMPILE FAILED: {res.getErrorMessage()}")

    decomp.dispose()
    proj.close(prog)
    proj.close()

except Exception as e:
    results.append(f"ERROR: {traceback.format_exc()}")

output = "\n".join(results)
print(output[:3000])  # print first 3000 chars
with open(OUT_FILE, "w", encoding="utf-8") as f:
    f.write(output)
print(f"\nFull output written to {OUT_FILE}")
