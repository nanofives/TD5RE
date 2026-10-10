"""[R1015 C] Find meshes in a level's MODELS.DAT by (nvtx, ncmd) and/or first page."""
import struct, sys
d, nv, nc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
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
        if nvtx == nv and ncmd == nc:
            r, cx, cy, cz = struct.unpack_from("<ffff", b, m + 12)
            cmdoff = struct.unpack_from("<I", b, m + 0x2C)[0]
            pg = [struct.unpack_from("<HHIHHI", b, m + cmdoff + c * 16)[1] for c in range(ncmd)]
            print("e%d s%d kind=%d c=(%.0f,%.0f,%.0f) r=%.0f pages=%s" % (e, s, t[20 + e * stride + s], cx, cy, cz, r, pg))
