# List all functions in gap regions
# @category TD5RE

fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory()

gap_regions = [
    ("0x414F40", "0x4183AF"),
    ("0x41C320", "0x41D840"),
    ("0x41D890", "0x421D9F"),
]

results = []
for start_str, end_str in gap_regions:
    start = af.getAddress(start_str)
    end = af.getAddress(end_str)
    it = fm.getFunctions(start, True)
    while it.hasNext():
        f = it.next()
        ep = f.getEntryPoint()
        if ep.compareTo(end) > 0:
            break
        name = f.getName()
        needs_naming = name.startswith("FUN_")
        results.append("%s|%s|%s" % (str(ep), name, "NEEDS_NAME" if needs_naming else "HAS_NAME"))

with open("C:/Users/maria/Desktop/Proyectos/TD5RE/re/scripts/gap_functions_list.txt", "w") as f:
    f.write("Total: %d\n" % len(results))
    for r in results:
        f.write(r + "\n")

print("Listed %d functions" % len(results))
