"""
Investigate frontend slideshow behavior in TD5_d3d.exe via pyghidra.
Uses the existing analyzed project at pyghidra_projects/my_project.
"""
import pyghidra
import sys
import os

GHIDRA_DIR = r"C:\Users\maria\Desktop\Proyectos\TD5RE\ghidra_12.0.3_PUBLIC"
BINARY     = r"C:\Users\maria\Desktop\Proyectos\TD5RE\original\TD5_d3d.exe"
PROJECT_DIR  = r"C:\Users\maria\Desktop\Proyectos\TD5RE\pyghidra_projects"
PROJECT_NAME = "my_project"

def addr(prog, va):
    return prog.getAddressFactory().getDefaultAddressSpace().getAddress(va)

def decomp_func(decomp_ifc, prog, va_str):
    from ghidra.app.decompiler import DecompileOptions
    a = prog.getAddressFactory().getDefaultAddressSpace().getAddress(va_str)
    fn = prog.getFunctionManager().getFunctionAt(a)
    if fn is None:
        return f"[no function at {va_str}]", None
    decomp_ifc.openProgram(prog)
    res = decomp_ifc.decompileFunction(fn, 60, None)
    code = res.getDecompiledFunction()
    if code:
        return fn.getName(), code.getC()
    return fn.getName(), "[decompile failed]"

def search_symbols(prog, keywords):
    """Search function names for keywords."""
    fm = prog.getFunctionManager()
    results = []
    for fn in fm.getFunctions(True):
        name_lower = fn.getName().lower()
        for kw in keywords:
            if kw in name_lower:
                results.append((fn.getName(), str(fn.getEntryPoint())))
                break
    return results

def search_strings(prog, keywords):
    """Search defined strings in the binary."""
    from ghidra.program.model.data import StringDataType
    from ghidra.program.model.listing import Data
    dt = prog.getDataTypeManager()
    listing = prog.getListing()
    results = []
    # iterate data
    di = listing.getDefinedData(True)
    for d in di:
        try:
            val = str(d.getValue())
            val_lower = val.lower()
            for kw in keywords:
                if kw in val_lower:
                    results.append((str(d.getAddress()), val[:80]))
                    break
        except:
            pass
    return results

pyghidra.start()

from ghidra.base.project import GhidraProject
proj = GhidraProject.openProject(PROJECT_DIR, PROJECT_NAME)
prog = proj.openProgram("/", "TD5_d3d.exe", False)

print(f"Program: {prog.getName()}")
print(f"Image base: {prog.getImageBase()}")
print()

# 1. Search function names
keywords = ["slide", "slideshow", "background", "cycle", "attract", "gallery",
            "frontend", "menu", "anim", "blit", "display"]
print("=== FUNCTION NAME SEARCH ===")
matches = search_symbols(prog, keywords)
for name, ea in sorted(matches):
    print(f"  {ea}  {name}")
print()

# 2. Search strings for slide/background/pic
print("=== STRING SEARCH (slide/pic/background/cycle) ===")
str_matches = search_strings(prog, ["slide", "pic", "background", "cycle", "attract", "gallery"])
for ea, val in str_matches:
    print(f"  {ea}  {repr(val)}")
print()

# 3. Decompile key functions
from ghidra.app.decompiler import DecompInterface, DecompileOptions
decomp = DecompInterface()
opts = DecompileOptions()
decomp.setOptions(opts)
decomp.openProgram(prog)

# Key functions to decompile (from td5_sdk.h + known frontend addresses)
TARGETS = [
    ("0x00415490", "ScreenMainMenuAnd1PRaceFlow"),
    ("0x004275A0", "RunAttractModeDemoScreen"),
    ("0x00414B50", "RunFrontendDisplayLoop"),
    ("0x00417D50", "ScreenExtrasGallery"),
    ("0x00414610", "SetFrontendScreen"),
]

for va, label in TARGETS:
    try:
        a = prog.getAddressFactory().getDefaultAddressSpace().getAddress(va)
        fn = prog.getFunctionManager().getFunctionAt(a)
        if fn is None:
            # Try nearby
            fn = prog.getFunctionManager().getFunctionContaining(a)
        if fn is None:
            print(f"\n=== {label} @ {va}: NO FUNCTION ===")
            continue
        res = decomp.decompileFunction(fn, 60, None)
        code = res.getDecompiledFunction()
        src = code.getC() if code else "[failed]"
        print(f"\n=== {label} @ {va} (actual: {fn.getEntryPoint()}) ===")
        # Print first 100 lines
        lines = src.split('\n')
        for l in lines[:100]:
            print(l)
        if len(lines) > 100:
            print(f"  ... ({len(lines)-100} more lines)")
    except Exception as e:
        print(f"\n=== {label} @ {va}: ERROR {e} ===")

proj.close()
