# Ghidra script to find and create functions in gap regions
# @category TD5RE

from ghidra.program.model.address import AddressSet
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd

prog = currentProgram
af = prog.getAddressFactory()
fm = prog.getFunctionManager()
listing = prog.getListing()
mem = prog.getMemory()
refmgr = prog.getReferenceManager()

gap_regions = [
    ("0x414F40", "0x4183AF"),
    ("0x41C320", "0x41D840"),
    ("0x41D890", "0x421D9F"),
]

results = []

for start_str, end_str in gap_regions:
    start = af.getAddress(start_str)
    end = af.getAddress(end_str)

    # Disassemble the region
    addr_set = AddressSet(start, end)
    cmd = DisassembleCommand(addr_set, None, True)
    cmd.applyTo(prog)

    # Find function starts: after RET+padding, and CALL targets
    func_starts = set()

    # Add start of region if no function there
    if fm.getFunctionAt(start) is None:
        func_starts.add(start)

    # Scan for RET followed by code
    addr = start
    while addr is not None and addr.compareTo(end) <= 0:
        inst = listing.getInstructionAt(addr)
        if inst is None:
            addr = addr.add(1)
            continue

        mn = inst.getMnemonicString()
        if mn == "RET":
            next_addr = inst.getAddress().add(inst.getLength())
            # Skip padding (NOP=0x90, INT3=0xCC, align bytes)
            while next_addr.compareTo(end) <= 0:
                try:
                    b = mem.getByte(next_addr) & 0xFF
                except:
                    break
                if b == 0x90 or b == 0xCC or b == 0x00:
                    next_addr = next_addr.add(1)
                else:
                    break
            if next_addr.compareTo(end) <= 0:
                ni = listing.getInstructionAt(next_addr)
                if ni is not None and fm.getFunctionAt(next_addr) is None:
                    func_starts.add(next_addr)

        addr = inst.getAddress().add(inst.getLength())

    # Also find CALL targets into this region
    addr = start
    while addr.compareTo(end) <= 0:
        refs = refmgr.getReferencesTo(addr)
        for ref in refs:
            rt = ref.getReferenceType()
            if rt.isCall():
                if fm.getFunctionAt(addr) is None:
                    func_starts.add(addr)
                break
        addr = addr.add(1)

    # Create functions at all found starts
    for fs in sorted(func_starts):
        if fm.getFunctionAt(fs) is None:
            cmd2 = CreateFunctionCmd(fs)
            cmd2.applyTo(prog)
            f = fm.getFunctionAt(fs)
            if f is not None:
                results.append(str(fs))

# Write results
with open("C:/Users/maria/Desktop/Proyectos/TD5RE/re/scripts/gap_functions_result.txt", "w") as fout:
    fout.write("Functions created: %d\n" % len(results))
    for r in results:
        fout.write(r + "\n")

print("Done. Created %d functions." % len(results))
