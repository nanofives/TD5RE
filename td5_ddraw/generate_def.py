"""
Generate ddraw.def. Every export except DirectDrawCreate is forwarded to
ddraw_real.dll (a copy of the system ddraw.dll alongside the proxy).
DirectDrawCreate is provided by our own source so we can intercept it.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OBJDUMP = os.path.abspath(
    os.path.join(HERE, "..", "td5mod", "deps", "mingw", "mingw32", "bin", "objdump.exe")
)
SYSTEM_DLL = r"C:\Windows\SysWOW64\ddraw.dll"
OUT_DEF = os.path.join(HERE, "src", "ddraw.def")

HOOKED = {"DirectDrawCreate"}


def main():
    out = subprocess.check_output([OBJDUMP, "-p", SYSTEM_DLL], text=True)
    exports = []
    in_table = False
    for line in out.splitlines():
        if "[Ordinal/Name Pointer] Table" in line:
            in_table = True
            continue
        if not in_table:
            continue
        m = re.search(r"\+base\[\s*\d+\]\s+\S+\s+(\S+)\s*$", line)
        if m:
            exports.append(m.group(1))
        elif line.strip() == "" and exports:
            break

    os.makedirs(os.path.dirname(OUT_DEF), exist_ok=True)
    with open(OUT_DEF, "w", newline="\n") as f:
        f.write("LIBRARY ddraw\n")
        f.write("EXPORTS\n")
        for name in exports:
            if name in HOOKED:
                f.write(f"    {name}\n")  # resolved from our source
            else:
                f.write(f"    {name} = ddraw_real.{name}\n")
    print(f"Wrote {OUT_DEF} ({len(exports)} exports, {len(HOOKED)} hooked)")


if __name__ == "__main__":
    main()
