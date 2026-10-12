"""[ROUND 1024 G] Offline top-down coverage of a world rectangle by street surface quads of level091's MODELS.DAT.

  python -I verify/r1024g_gapcover.py <dir with MODELS.DAT + MESHTAG.BIN> x0 x1 z0 z1 entry [entry ...]

Reports the fraction of the rectangle covered by asphalt (page 101 / 0 / 121) and pavement (page 44) quads of the given
MODELS.DAT entries (span >> 2), 100-unit cells. Used to measure the 5878-unit hole between street edge 8 and backstreet edge
67 (x 114200..119000, z -27150..-21400, entries 19 18 20): 0.0% covered with TD5RE_GEO_STREET_CONNECT=0, 96.6% with it on.
"""
import struct, sys, json
KINDS = ["other","skirt","road","branch-road","tunnel","gantry","end-wall","deck","water","coast","decal","city","block","cross","flora","park-tree","terrain","building","prop","rail","branch-side"]
HDR, CMD, VTX = 0x38, 16, 44
class Model:
    def __init__(self, path, tagpath=None):
        self.b = open(path,'rb').read()
        self.tag = open(tagpath,'rb').read() if tagpath else None
        (self.nent,) = struct.unpack_from('<I', self.b, 0)
    def entry_meshes(self, entry):
        b=self.b
        eoff,esize = struct.unpack_from('<II', b, 4+entry*8)
        (nm,) = struct.unpack_from('<I', b, eoff)
        out=[]
        for i in range(nm):
            (mo,) = struct.unpack_from('<I', b, eoff+4+i*4)
            m=eoff+mo
            magic,flags,ncmd,nvtx = struct.unpack_from('<HHII', b, m)
            radius,cx,cy,cz = struct.unpack_from('<ffff', b, m+12)
            cmdoff,=struct.unpack_from('<I',b,m+0x2C); vtxoff,=struct.unpack_from('<I',b,m+0x30)
            cmds=[]; 
            for c in range(ncmd):
                _d,page,_z,tri,quad,_z2 = struct.unpack_from('<HHIHHI', b, m+cmdoff+c*CMD)
                cmds.append((page,tri,quad))
            vtx=[struct.unpack_from('<fff', b, m+vtxoff+v*VTX) for v in range(nvtx)]
            kind=None
            if self.tag and 4+(entry*256+i) < len(self.tag):
                k=self.tag[20+entry*256+i] if len(self.tag)>20+entry*256+i else None
                kind = KINDS[k] if k is not None and k<len(KINDS) else k
            out.append(dict(i=i,nv=nvtx,ncmd=ncmd,cmds=cmds,r=radius,c=(cx,cy,cz),vtx=vtx,kind=kind,entry=entry))
        return out
def quads(mesh):
    # de-indexed: 4 verts per quad in order of commands (quads first? assume per cmd tri*3 then quad*4)
    v=mesh['vtx']; p=0; out=[]
    for page,tri,quad in mesh['cmds']:
        for t in range(tri):
            out.append((page,v[p:p+3])); p+=3
        for q in range(quad):
            out.append((page,v[p:p+4])); p+=4
    return out


import sys

d = sys.argv[1]
x0, x1, z0, z1 = [float(v) for v in sys.argv[2:6]]
ents = [int(v) for v in sys.argv[6:]]
CELL = 100.0
W = int((x1 - x0) / CELL); H = int((z1 - z0) / CELL)
M = Model(d + '/MODELS.DAT', d + '/MESHTAG.BIN')
cov = [bytearray(W) for _ in range(H)]
pagecnt = {}
for e in ents:
    for m in M.entry_meshes(e):
        if m['kind'] not in ('city', 'cross', 'road', 'block', 'branch-road'):
            continue
        for page, q in quads(m):
            if page not in (101, 44, 0, 121):
                continue
            pts = [((x - x0) / CELL, (z - z0) / CELL) for x, _y, z in q]
            ymin = max(0, int(min(p[1] for p in pts))); ymax = min(H - 1, int(max(p[1] for p in pts)))
            n = len(pts)
            for iy in range(ymin, ymax + 1):
                yy = iy + 0.5; xs = []
                for i in range(n):
                    a = pts[i]; b = pts[(i + 1) % n]
                    if (a[1] <= yy < b[1]) or (b[1] <= yy < a[1]):
                        xs.append(a[0] + (yy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
                xs.sort()
                for k in range(0, len(xs) - 1, 2):
                    for ix in range(max(0, int(xs[k])), min(W - 1, int(xs[k + 1])) + 1):
                        cov[iy][ix] = 1
tot = W * H
hit = sum(sum(r) for r in cov)
print('%s: covered %d / %d cells = %.1f%%  (uncovered %.0f m2 at 430 u/m)' % (d, hit, tot, 100.0 * hit / tot, (tot - hit) * CELL * CELL / (430.0 * 430.0)))
