"""
Decompile FUN_00424560 and FUN_00425b60 from TD5_d3d.exe using pyghidra headless.
Run from project root.
"""
import pyghidra
import sys
import os

GHIDRA_INSTALL = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
BINARY = r"C:\Users\maria\Desktop\Proyectos\TD5RE\original\TD5_d3d.exe"
PROJECT_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\decomp_tmp"
PROJECT_NAME = "TD5_decomp_tmp"
OUT_FILE = r"C:\Users\maria\Desktop\Proyectos\TD5RE\re\tools\decomp_buttons_out.txt"

TARGETS = ["0x00424560", "0x00425b60"]

os.makedirs(PROJECT_DIR, exist_ok=True)

pyghidra.start(install_dir=GHIDRA_INSTALL)

with pyghidra.open_program(BINARY, project_location=PROJECT_DIR, project_name=PROJECT_NAME, analyze=False) as flat_api:
    from ghidra.app.decompiler import DecompInterface, DecompileOptions
    prog = flat_api.getCurrentProgram()
    fm = prog.getFunctionManager()
    addr_factory = prog.getAddressFactory()

    decomp = DecompInterface()
    opts = DecompileOptions()
    decomp.setOptions(opts)
    decomp.openProgram(prog)

    results = []
    for addrStr in TARGETS:
        addr = addr_factory.getAddress(addrStr)
        fn = fm.getFunctionAt(addr)
        results.append(f"=== {addrStr} ===")
        if fn is None:
            results.append("NO FUNCTION AT THIS ADDRESS\n")
            continue
        results.append(f"Name: {fn.getName()}")
        from ghidra.util.task import ConsoleTaskMonitor
        monitor = ConsoleTaskMonitor()
        res = decomp.decompileFunction(fn, 60, monitor)
        if res.decompileCompleted():
            results.append(res.getDecompiledFunction().getC())
        else:
            results.append(f"FAILED: {res.getErrorMessage()}")
        results.append("")

    decomp.dispose()

    output = "\n".join(results)
    print(output)
    with open(OUT_FILE, "w") as f:
        f.write(output)
    print(f"\nWritten to {OUT_FILE}")
