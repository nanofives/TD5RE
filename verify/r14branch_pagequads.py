"""[R14 BRANCH] Per-page quad histogram over a whole MODELS.DAT.

Byte-level attribution for item 2b: the outer face is one extra quad per paved
span-side on the pavement page and must move NOTHING else. Prints the diff
between two files so "which page gained quads, and how many" is a number read
out of the file rather than a claim about the emitter.

Format is tg_emit_models (see verify/r12geom_dumpmodels.py for the field map).

Usage: python r14branch_pagequads.py <A.DAT> <B.DAT>
"""
import struct, sys, collections

HDR, CMD, VTX = 0x38, 16, 44


def hist(path):
    b = open(path, 'rb').read()
    (nent,) = struct.unpack_from('<I', b, 0)
    q = collections.Counter()
    meshes = 0
    for e in range(nent):
        eoff, esize = struct.unpack_from('<II', b, 4 + e * 8)
        if esize == 0 or eoff + 4 > len(b):
            continue
        (nmesh,) = struct.unpack_from('<I', b, eoff)
        for i in range(nmesh):
            (mo,) = struct.unpack_from('<I', b, eoff + 4 + i * 4)
            m = eoff + mo
            if m + HDR > len(b):
                continue
            magic, flags, ncmd, nvtx = struct.unpack_from('<HHII', b, m)
            if magic != 259:
                continue
            (cmdoff,) = struct.unpack_from('<I', b, m + 0x2C)
            meshes += 1
            for c in range(ncmd):
                co = m + cmdoff + c * CMD
                if co + CMD > len(b):
                    break
                _d, page, _z, tri, quad, _z2 = struct.unpack_from('<HHIHHI', b, co)
                q[page] += quad
    return q, meshes, len(b)


def main():
    a, am, asz = hist(sys.argv[1])
    c, cm, csz = hist(sys.argv[2])
    print("A %s  %d bytes  %d meshes  %d quads" % (sys.argv[1], asz, am, sum(a.values())))
    print("B %s  %d bytes  %d meshes  %d quads" % (sys.argv[2], csz, cm, sum(c.values())))
    print("page   A       B       delta")
    for p in sorted(set(a) | set(c)):
        if a[p] != c[p]:
            print("%-6d %-7d %-7d %+d" % (p, a[p], c[p], c[p] - a[p]))
    print("unchanged pages: %d" % sum(1 for p in set(a) | set(c) if a[p] == c[p]))


main()
