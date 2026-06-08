"""Re-extract SNK_RaceTypeText (multi-line, NUL-walking) from the English Language.dll.
SNK_RaceTypeText is an array of char* (indexed by game_type); each points to a MULTI-LINE
string (NUL-separated lines, terminated by an empty line / double-NUL). The old dumper
truncated at the first NUL, so k_race_desc was reconstructed and is untrusted. This walks
past NULs to recover the real lines."""
import struct

DLL = r"C:/Users/maria/Desktop/Proyectos/TD5RE/original/Language/English/Language.dll"
data = open(DLL, "rb").read()
pe = struct.unpack_from("<I", data, 0x3c)[0]
coff = pe + 4
nsec = struct.unpack_from("<H", data, coff + 2)[0]
optsz = struct.unpack_from("<H", data, coff + 16)[0]
opt = coff + 20
magic = struct.unpack_from("<H", data, opt)[0]
imgbase = struct.unpack_from("<I", data, opt + (28 if magic == 0x10b else 24))[0]
ddir = opt + (96 if magic == 0x10b else 112)
exp_rva = struct.unpack_from("<I", data, ddir)[0]
sect = opt + optsz
secs = []
for i in range(nsec):
    o = sect + i * 40
    vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((va, vsz, raw, rsz))


def r2o(rva):
    for va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            off = raw + (rva - va)
            return off if off < len(data) else None
    return None


def cstr(off):
    if off is None:
        return None
    e = data.find(b"\0", off)
    return data[off:e].decode("latin1", "replace")


eo = r2o(exp_rva)
nname = struct.unpack_from("<I", data, eo + 24)[0]
fo = r2o(struct.unpack_from("<I", data, eo + 28)[0])
no = r2o(struct.unpack_from("<I", data, eo + 32)[0])
oo = r2o(struct.unpack_from("<I", data, eo + 36)[0])
exp = {}
for i in range(nname):
    nm = cstr(r2o(struct.unpack_from("<I", data, no + i * 4)[0]))
    ordi = struct.unpack_from("<H", data, oo + i * 2)[0]
    exp[nm] = struct.unpack_from("<I", data, fo + ordi * 4)[0]

# Find the SNK_RaceTypeText export (mangled name like ?SNK_RaceTypeText@@3PA...)
rtt = None
for nm, rva in exp.items():
    if "RaceTypeText" in nm:
        rtt = (nm, rva)
        break
print("export:", rtt)
if not rtt:
    raise SystemExit("SNK_RaceTypeText not found")

base = r2o(rtt[1])


def lines_at(rva):
    """Walk NUL-separated lines until an empty line (double-NUL)."""
    off = r2o(rva)
    if off is None:
        return None
    out = []
    cur = off
    while cur < len(data) and len(out) < 20:
        e = data.find(b"\0", cur)
        seg = data[cur:e].decode("latin1", "replace")
        if seg == "":
            break
        out.append(seg)
        cur = e + 1
    return out


# The array holds pointers. They may be VAs (imgbase+rva) or RVAs. Detect.
for gt in range(12):
    ptr = struct.unpack_from("<I", data, base + gt * 4)[0]
    if ptr == 0:
        print("gt %2d: <null>" % gt)
        continue
    rva = ptr - imgbase if ptr >= imgbase else ptr
    print("gt %2d (ptr=0x%08X rva=0x%X): %r" % (gt, ptr, rva, lines_at(rva)))
