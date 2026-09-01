"""[R12 GEOM] Offline MODELS.DAT mesh dump -- objective attribution.

Format (tg_emit_models):
  file: u32 nentries, then nentries x (u32 offset, u32 size), then the blocks.
  block: u32 nmesh, nmesh x u32 mesh-offset (relative to the block start),
         then the mesh records.
  mesh:  0x38 header -- u16 magic(259) u16 flags u32 ncmd u32 nvtx f32 radius
         f32 cx cy cz  f32 x3  u32  u32 cmdoff  u32 vtxoff  u32
         cmds: ncmd x 16 bytes -- u16 dispatch u16 page u32 u16 tri u16 quad u32
         vtx:  nvtx x 44 bytes -- f32 x y z, f32 x3, u32 argb, f32 u v, f32 x2

Usage: python r12geom_dumpmodels.py <MODELS.DAT> <entry-span> [spans-per-entry]
"""
import struct, sys, collections

SPE = 4                # TD5_TG_SPANS_PER_ENTRY (override via argv[3])
HDR, CMD, VTX = 0x38, 16, 44


def rd(fmt, b, o):
    return struct.unpack_from(fmt, b, o)


def dump(path, span, spe):
    b = open(path, 'rb').read()
    (nent,) = rd('<I', b, 0)
    entry = span // spe
    if entry >= nent:
        print("entry %d out of range (%d entries)" % (entry, nent))
        return
    eoff, esize = rd('<II', b, 4 + entry * 8)
    print("MODELS.DAT %d bytes, %d entries; span %d -> entry %d "
          "(spans %d..%d) @%d size %d"
          % (len(b), nent, span, entry, entry * spe, entry * spe + spe - 1,
             eoff, esize))
    (nmesh,) = rd('<I', b, eoff)
    print("  %d meshes" % nmesh)
    rows = []
    for i in range(nmesh):
        (mo,) = rd('<I', b, eoff + 4 + i * 4)
        m = eoff + mo
        magic, flags, ncmd, nvtx = rd('<HHII', b, m)
        radius, cx, cy, cz = rd('<ffff', b, m + 12)
        cmdoff, vtxoff = rd('<II', b, m + 0x2C), rd('<I', b, m + 0x30)
        cmdoff = cmdoff[0]
        vtxoff = vtxoff[0]
        pages, quads, tris = [], 0, 0
        for c in range(ncmd):
            co = m + cmdoff + c * CMD
            _d, page, _z, tri, quad, _z2 = rd('<HHIHHI', b, co)
            pages.append(page)
            quads += quad
            tris += tri
        xs, ys, zs = [], [], []
        for v in range(nvtx):
            vx, vy, vz = rd('<fff', b, m + vtxoff + v * VTX)
            xs.append(vx); ys.append(vy); zs.append(vz)
        rows.append(dict(i=i, off=mo, magic=magic, flags=flags, nv=nvtx,
                         quads=quads, tris=tris, pages=pages,
                         bx=(min(xs), max(xs)), by=(min(ys), max(ys)),
                         bz=(min(zs), max(zs)),
                         c=(cx, cy, cz), r=radius))
    for r in rows:
        print("  #%-3d off=%-7d nv=%-3d q=%-2d t=%-2d pages=%-14s "
              "x[%9.0f,%9.0f] y[%8.0f,%8.0f] z[%9.0f,%9.0f]"
              % (r['i'], r['off'], r['nv'], r['quads'], r['tris'],
                 ",".join(str(p) for p in r['pages']),
                 r['bx'][0], r['bx'][1], r['by'][0], r['by'][1],
                 r['bz'][0], r['bz'][1]))
    # Coincidence report: meshes whose bounding boxes are (near) identical are
    # the objective signature of "the same thing emitted twice".
    print("  ---- near-identical bounding boxes (tol 60 units) ----")
    dup = 0
    for a in range(len(rows)):
        for c in range(a + 1, len(rows)):
            ra, rc = rows[a], rows[c]
            d = max(abs(ra[k][j] - rc[k][j])
                    for k in ('bx', 'by', 'bz') for j in (0, 1))
            if d <= 60.0:
                dup += 1
                print("    #%d ~ #%d  (max corner delta %.0f) pages %s / %s"
                      % (ra['i'], rc['i'], d, ra['pages'], rc['pages']))
    print("    %d coincident pair(s)" % dup)


if __name__ == '__main__':
    dump(sys.argv[1], int(sys.argv[2]),
         int(sys.argv[3]) if len(sys.argv) > 3 else SPE)
