"""[R1015 C] List meshes that use any page of a given set."""
import struct, sys
d = sys.argv[1]; want = set(int(x) for x in sys.argv[2].split(","))
b = open(d + "/MODELS.DAT", "rb").read()
t = open(d + "/MESHTAG.BIN", "rb").read()
stride = struct.unpack_from("<I", t, 16)[0]
nent = struct.unpack_from("<I", b, 0)[0]
for e in range(nent):
    eoff, _ = struct.unpack_from("<II", b, 4 + e * 8)
    nm = struct.unpack_from("<I", b, eoff)[0]
    for s in range(nm):
        mo = struct.unpack_from("<I", b, eoff + 4 + s * 4)[0]
        m = eoff + mo
        _mg, fl, ncmd, nvtx = struct.unpack_from("<HHII", b, m)
        r, cx, cy, cz = struct.unpack_from("<ffff", b, m + 12)
        cmdoff = struct.unpack_from("<I", b, m + 0x2C)[0]
        pg = [struct.unpack_from("<HHIHHI", b, m + cmdoff + c * 16)[1] for c in range(ncmd)]
        if want & set(pg):
            print("e%d s%d kind=%d c=(%.0f,%.0f,%.0f) r=%.0f v=%d c=%d pages=%s" % (e, s, t[20 + e * stride + s], cx, cy, cz, r, nvtx, ncmd, pg))
