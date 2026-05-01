"""
Generate winmm.def listing every export from the system winmm.dll and
forwarding it to winmm_real.dll (a copy of the system winmm alongside the
proxy). MinGW's linker turns `name = winmm_real.name` entries into PE
export forwarders, so the proxy itself carries zero code for those exports.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OBJDUMP = os.path.abspath(
    os.path.join(HERE, "..", "td5mod", "deps", "mingw", "mingw32", "bin", "objdump.exe")
)
SYSTEM_DLL = r"C:\Windows\SysWOW64\winmm.dll"
OUT_DEF = os.path.join(HERE, "src", "winmm.def")


def main():
    if not os.path.exists(OBJDUMP):
        print(f"ERROR: objdump not found at {OBJDUMP}")
        sys.exit(1)
    if not os.path.exists(SYSTEM_DLL):
        print(f"ERROR: {SYSTEM_DLL} not found")
        sys.exit(1)

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

    if not exports:
        print("ERROR: no exports parsed")
        sys.exit(1)

    os.makedirs(os.path.dirname(OUT_DEF), exist_ok=True)
    with open(OUT_DEF, "w", newline="\n") as f:
        f.write("LIBRARY winmm\n")
        f.write("EXPORTS\n")
        for name in exports:
            f.write(f"    {name} = winmm_real.{name}\n")

    print(f"Wrote {OUT_DEF} ({len(exports)} forwarders)")


if __name__ == "__main__":
    main()
