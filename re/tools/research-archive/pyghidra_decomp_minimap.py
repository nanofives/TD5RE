#!/usr/bin/env python3
"""Use pyghidra directly to decompile TD5_d3d.exe minimap functions."""
import pyghidra
import sys

BINARY = "C:/Users/maria/Desktop/Proyectos/TD5RE/original/TD5_d3d.exe"
GHIDRA_DIR = "C:/Users/maria/Desktop/Proyectos/TD5RE/ghidra_12.0.3_PUBLIC"
import os
os.environ["GHIDRA_INSTALL_DIR"] = GHIDRA_DIR

FUNCS = [
    (0x43B0A0, "InitMinimapLayout"),
    (0x43A220, "RenderTrackMinimapOverlay"),
]

def main():
    print(f"Opening {BINARY} with pyghidra...")
    with pyghidra.open_program(BINARY,
            project_location="C:/Users/maria/Desktop/Proyectos/TD5RE/re/tools/tmp_ghidra_proj",
            project_name="TD5_decomp_tmp",
            analyze=False) as flat_api:
        print("Program opened.")
        from ghidra.app.decompiler import DecompInterface
        from ghidra.util.task import ConsoleTaskMonitor

        program = flat_api.getCurrentProgram()
        addr_factory = program.getAddressFactory()
        func_mgr = program.getFunctionManager()

        # Open one decompiler instance for all functions
        di = DecompInterface()
        di.openProgram(program)
        monitor = ConsoleTaskMonitor()

        for addr_int, name in FUNCS:
            print(f"\n{'='*70}")
            print(f"Function: {name} @ 0x{addr_int:08X}")
            print(f"{'='*70}")
            addr = addr_factory.getAddress(f"0x{addr_int:08X}")
            func = func_mgr.getFunctionAt(addr)
            if func is None:
                print(f"  No function at address. Searching containing function...")
                func = func_mgr.getFunctionContaining(addr)
            if func is None:
                print(f"  ERROR: No function found at or containing 0x{addr_int:08X}")
                continue
            print(f"  Function name: {func.getName()}")
            print(f"  Entry: {func.getEntryPoint()}")

            result = di.decompileFunction(func, 120, monitor)
            if result and result.decompileCompleted():
                dc = result.getDecompiledFunction()
                if dc:
                    print(dc.getC())
                else:
                    print("  No C output")
            else:
                print("  Decompile failed:", result.getErrorMessage() if result else "null result")

        di.closeProgram()

main()
