"""[R1015 C] Decode free-cam picks (entry, slot) of a generated level into mesh
header + MESHTAG kind + page list + vertex bbox.
  python r1015c_decode.py <levelDir> e:s [e:s ...]
"""
import struct, sys
KIND = ["other","skirt","road","branch-road","tunnel","gantry","end-wall","deck",
        "water","coast","decal","city","block","cross","flora","park-tree",
        "terrain","building","prop","rail","branch-side"]
def main():
    d = sys.argv[1]
    b = open(d + "/MODELS.DAT", "rb").read()
    t = open(d + "/MESHTAG.BIN", "rb").read()
    magic, ver, seed, nent, stride = struct.unpack_from("<5I", t, 0)
    for arg in sys.argv[2:]:
        e, s = map(int, arg.split(":"))
        eoff, esz = struct.unpack_from("<II", b, 4 + e * 8)
        nm = struct.unpack_from("<I", b, eoff)[0]
        mo = struct.unpack_from("<I", b, eoff + 4 + s * 4)[0]
        m = eoff + mo
        _mg, fl, ncmd, nvtx = struct.unpack_from("<HHII", b, m)
        r, cx, cy, cz = struct.unpack_from("<ffff", b, m + 12)
        cmdoff = struct.unpack_from("<I", b, m + 0x2C)[0]
        vtxoff = struct.unpack_from("<I", b, m + 0x30)[0]
        pages = []
        for c in range(ncmd):
            _d, pg, _z, tri, quad, _z2 = struct.unpack_from("<HHIHHI", b, m + cmdoff + c * 16)
            pages.append((pg, tri, quad))
        k = t[20 + e * stride + s] if 20 + e * stride + s < len(t) else 255
        print("e%d s%d (of %d) kind=%s c=(%.0f,%.0f,%.0f) r=%.0f v=%d cmds=%s" %
              (e, s, nm, KIND[k] if k < len(KIND) else k, cx, cy, cz, r, nvtx, pages))
        xs = [];ys = [];zs = []
        for v in range(nvtx):
            x, y, z = struct.unpack_from("<fff", b, m + vtxoff + v * 44)
            xs.append(x); ys.append(y); zs.append(z)
        print("   bbox x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]" %
              (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
main()
