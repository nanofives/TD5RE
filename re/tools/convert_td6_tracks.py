#!/usr/bin/env python3
"""
convert_td6_tracks.py — migrate Test Drive 6 tracks into the TD5RE source port.

Strategy A (offline transcode): read a TD6 level (+ its texture archive) and emit a
TD5-shaped loose-file level directory at re/assets/levels/level<NNN>/ that the faithful
TD5 engine loads via its existing loose-file override path (td5_asset.c
load_first_available_level_entry checks re/assets/levels/level%03d/<FILE> before the zip).

This is the sibling of convert_td6_cars.py. See re/analysis/td6_track_migration_plan.md.

TD6 vs TD5 format facts (verified by hexdump, 2026-06-02):
  - strip.dat / stripb.dat : SAME 5xu32 header + 24B spans + 6B verts as TD5, PLUS an
    extra metadata block inserted between the header (ends 0x14) and the span table
    (span_offset, e.g. 0x188). The TD5 parser uses ABSOLUTE span_offset/vertex_offset
    (td5_track.c:2109), so the block is skipped cleanly -> strip.dat is passthrough.
    NB: the TD5 parser also reads a "jump table" count at 0x14; TD6 puts a nonzero
    value there, so TD6's block is consumed as jump entries (may be benign).
  - models.dat : SAME (size, cumulative-offset) mesh directory; meshes are render_type
    byte 0x04 (indexed: 32B verts pos+nrm+uv, u16 tri-list) vs TD5 0x03 (expanded 44B
    per-corner verts). Mesh header is the SAME 0x38 layout. -> Milestone B de-indexes.
  - textures : externalized to LEVELS/texture*.zip keyed by a 64B-record textures.dir,
    not a bundled textures.dat. -> Milestone B repacks.
  - levelinf.dat / checkpt.num : ABSENT in TD6 (folded into the strip block). -> synth.
  - left/right.trk : replaced by spline1-4.td6. traffic.bus -> .trf. -> Milestone B.

MILESTONE A (this version): strip + stripb passthrough + synthesized levelinf.dat.
  Drives the TD6 spline network on the faithful engine with PLACEHOLDER road geometry
  (no models.dat -> td5_track build_placeholder_display_lists). Tests the riskiest
  unknown: does a TD6 strip.dat drive on the TD5 engine?
MILESTONE B (stubbed below): de-index models.dat 0x04->0x03, repack textures.dat,
  synth checkpt.num, splines -> left/right.trk.
"""
import argparse
import io
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TD6_DIR = os.path.join(ROOT, "Test Drive 6")
TD6_LEVELS = os.path.join(TD6_DIR, "LEVELS")
ASSETS_LEVELS = os.path.join(ROOT, "re", "assets", "levels")

STRIP_HEADER_DWORDS = 5  # {span_offset, span_count, vertex_offset, vertex_count, total_spans}
SPAN_STRIDE = 24
VERT_STRIDE = 6
MESH_HEADER_SZ = 0x38


# --------------------------------------------------------------------------- utils
def td6_level_zip(level_num: int) -> str:
    return os.path.join(TD6_LEVELS, f"level{level_num:03d}.zip")


def read_zip_entry(zip_path: str, name: str):
    """Case-insensitive read of an entry from a zip; returns bytes or None."""
    if not os.path.exists(zip_path):
        return None
    with zipfile.ZipFile(zip_path) as z:
        lut = {n.lower(): n for n in z.namelist()}
        real = lut.get(name.lower())
        if real is None:
            return None
        return z.read(real)


def strip_header(data: bytes):
    if len(data) < STRIP_HEADER_DWORDS * 4:
        return None
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", data, 0)
    span_table_spans = (vtx_off - span_off) // SPAN_STRIDE if vtx_off > span_off else 0
    return dict(span_off=span_off, span_cnt=span_cnt, vtx_off=vtx_off,
                vtx_cnt=vtx_cnt, total=total, phys_spans=span_table_spans,
                jump_count_at_0x14=struct.unpack_from("<I", data, 0x14)[0] if len(data) >= 0x18 else 0)


def out_dir(td5_level_num: int) -> str:
    return os.path.join(ASSETS_LEVELS, f"level{td5_level_num:03d}")


# ============================================================ models.dat (Milestone B)
# Offline de-index: TD6 render_type 0x04 INDEXED meshes -> TD5 render_type 0x03
# EXPANDED meshes, mirroring the car-port runtime transcoder
# (td5_asset_transcode_td6_mesh on branch fix-1780437860) so the faithful track
# loader (td5_track_parse_models_dat) consumes them with ZERO engine change.
#
# Verified layouts (hexdump 2026-06-02):
#   TD6 0x04 mesh header (0x38, same as TD5): [+0]render_type u8=0x04, [+1]tex_page u8,
#     [+4]cmd_count i32, [+8]vert_count i32, [+0xC]radius f32, [+0x10..0x24]centers+origin,
#     [+0x2C]cmds_off u32, [+0x30]verts_off u32, [+0x34]norms_off (unused).
#   TD6 0x04 command (24B): [+8]vert_count, [+0xC]index_count(=tris*3), [+0x10]vert_off,
#     [+0x14]index_off.  index buffer = u16[index_count] tri-list.
#   TD6 0x04 vertex (32B): pos.xyz f32 @0 + (track: reserved/color, NOT a usable normal)
#     @0xC + uv.xy f32 @0x18.  -> we COMPUTE face normals (track verts carry 0,NaN,0).
#   TD5 0x03 mesh (out): header 0x38 + ONE TD5_PrimitiveCmd(16) + TD5_MeshVertex(44)*N
#     + TD5_VertexNormal(16)*N, all offsets MESH-relative (td5_track.c:5992).
MESH_HDR_FMT = "<BBHii fffffff IIII"   # render_type,tex,flags,cmd_count,vert_count,
                                       # radius,cx,cy,cz,ox,oy,oz, reserved,cmds,verts,norms
assert struct.calcsize(MESH_HDR_FMT) == 0x38
TD6_VSTRIDE = 32
# TD6 TRACK meshes use a 32-byte command (verified: (index_off-cmds_off)/cmd_count==32
# for every mesh in level010). NB the car himodel uses a 24-byte command — this
# converter is track-only. Field offsets within the command match: dispatch@+0,
# tex_page i16@+2, vert_count@+8, index_count@+0xC, vert_off@+0x10, index_off@+0x14.
TD6_CSTRIDE = 32
TD5_VSTRIDE = 44
TD5_NSTRIDE = 16
TD5_CSTRIDE = 16


def _u32(b, o): return struct.unpack_from("<I", b, o)[0]
def _i32(b, o): return struct.unpack_from("<i", b, o)[0]
def _f32(b, o): return struct.unpack_from("<f", b, o)[0]


def _detect_models_dir(blob):
    d0, d1 = struct.unpack_from("<2I", blob, 0)
    if 0 < d0 < 10000 and d1 == 4 + d0 * 8 and 4 + d0 * 8 <= len(blob):
        return d0, 4
    if 0 < d0 < 10000 and 4 + d0 * 8 <= len(blob) and d1 >= 4 + d0 * 8 and d1 < len(blob):
        return d0, 4
    if d1 and (d1 & 7) == 0 and d1 <= len(blob):
        return d1 // 8, 0
    return None, None


def _is_mesh_header(blob, o):
    """A TD6 0x04 track-mesh header: render_type byte 0x04, commands_offset==0x40,
    sane cmd/vert counts. Strong enough to scan for (a 0x04 byte inside vertex
    floats won't also have 0x40 at +0x2C and valid counts)."""
    if o + 0x38 > len(blob) or blob[o] != 0x04:
        return False
    if _u32(blob, o + 0x2C) != 0x40:
        return False
    cc = _i32(blob, o + 4); vc = _i32(blob, o + 8)
    return 1 <= cc <= 256 and 1 <= vc <= 65535


def _mesh_extent(blob, moff):
    """Byte offset just past the end of the 0x04 mesh (header + cmds + idx + verts)."""
    cmd_count = _i32(blob, moff + 4)
    cmds_off = moff + _u32(blob, moff + 0x2C)
    end = cmds_off + cmd_count * TD6_CSTRIDE
    for ci in range(min(cmd_count, 256)):
        cmd = cmds_off + ci * TD6_CSTRIDE
        if cmd + TD6_CSTRIDE > len(blob):
            break
        vcnt = _i32(blob, cmd + 0x08); idxc = _i32(blob, cmd + 0x0C)
        voff = moff + _i32(blob, cmd + 0x10); ioff = moff + _i32(blob, cmd + 0x14)
        if 0 < vcnt <= 65535:
            end = max(end, voff + vcnt * TD6_VSTRIDE)
        if 0 < idxc:
            end = max(end, ioff + idxc * 2)
    return end


def _scan_block_meshes(blob, eoff, blk_len):
    """Find every 0x04 mesh in a block by SCANNING, not by the offset table. The
    TD6 offset table is unreliable: slot 0 is a reserved sentinel (=0) and a
    single-mesh block stores moff=0 for a mesh that actually lives at +0x20, so
    the table-based walk silently drops ~37% of entries. Scanning + skipping each
    mesh's data extent recovers them all."""
    out = []
    end = min(eoff + blk_len, len(blob))
    o = eoff
    while o + 0x38 <= end:
        if _is_mesh_header(blob, o):
            out.append(o)
            nxt = _mesh_extent(blob, o)
            o = max(o + 0x38, (nxt + 3) & ~3)
        else:
            o += 4
    return out


def _iter_blocks(blob):
    """Yield (entry_index, [mesh_abs_offsets]) for every display-list block.
    Block bounds come from the directory table; mesh offsets within a block come
    from a SCAN (see _scan_block_meshes) rather than the unreliable slot table."""
    count, tbl = _detect_models_dir(blob)
    if count is None:
        return
    table_end = tbl + count * 8

    def is_off(f):
        return (table_end <= f < len(blob) and f + 4 <= len(blob)
                and 1 <= _u32(blob, f) <= 256)

    for i in range(count):
        off = tbl + i * 8
        if off + 8 > len(blob):
            break
        f0, f1 = struct.unpack_from("<2I", blob, off)
        if is_off(f0):
            eoff, esize = f0, f1
        elif is_off(f1):
            eoff, esize = f1, f0
        else:
            yield i, None
            continue
        if not (0 < esize <= len(blob) - eoff):
            esize = len(blob) - eoff
        meshes = _scan_block_meshes(blob, eoff, esize)
        yield i, (meshes if meshes else None)


def _pack_mesh(tris, billboard, ox, oy, oz):
    """Pack a TD5 render_type-0x03 mesh from a triangle list, with a TIGHT
    bounding sphere computed from the absolute vertex positions.

    billboard=False (road/buildings/walls): verts stay ABSOLUTE, origin =
    parent (ox,oy,oz), header tag = 0 -> the mesh NEVER faces the camera.
    billboard=True (trees): the renderer swaps in the yaw-stripped camera basis
    as the rotation, applied to the mesh's LOCAL verts, so verts must be
    relative to a pivot (absolute verts make the sprite swing around the world
    origin and swim as you drive). Verts are emitted relative to the bbox centre
    and origin is set to that centre (x256, the fixed-point the renderer divides
    out) so the sprite rotates in place. Bounding-sphere centre stays ABSOLUTE
    (the culler works in world space).

    Verts stay ABSOLUTE and origin is the parent's (ox,oy,oz), so geometry
    placement is byte-identical to the unsplit mesh — only the per-mesh
    bounding sphere (center @+0x10, radius @+0xC) tightens. Triangles are
    grouped by texture page into one TD5 command each (verts laid in command
    order for the renderer's sequential cursor).
    tris: list of (tex_page, [(px,py,pz,tu,tv) x3], (nx,ny,nz))."""
    if not tris:
        return None
    by_tp = {}
    for tp, p3, nrm in tris:
        by_tp.setdefault(tp, []).append((p3, nrm))
    cmds = list(by_tp.items())                 # [(tp, [(p3,nrm)...])...]
    n = sum(len(v) for _, v in cmds) * 3
    ncmd = len(cmds)
    out_cmds = 0x38
    out_verts = out_cmds + ncmd * TD5_CSTRIDE
    out_norms = out_verts + n * TD5_VSTRIDE
    total = out_norms + n * TD5_NSTRIDE
    buf = bytearray(total)
    # Tight bounding sphere: bbox centre + max radius over the absolute verts.
    minx = miny = minz = 1e30
    maxx = maxy = maxz = -1e30
    for tp, lst in cmds:
        for p3, nrm in lst:
            for v in p3:
                if v[0] < minx: minx = v[0]
                if v[0] > maxx: maxx = v[0]
                if v[1] < miny: miny = v[1]
                if v[1] > maxy: maxy = v[1]
                if v[2] < minz: minz = v[2]
                if v[2] > maxz: maxz = v[2]
    bcx, bcy, bcz = (minx + maxx) * 0.5, (miny + maxy) * 0.5, (minz + maxz) * 0.5
    radius = 0.0
    for tp, lst in cmds:
        for p3, nrm in lst:
            for v in p3:
                d = ((v[0] - bcx) ** 2 + (v[1] - bcy) ** 2 + (v[2] - bcz) ** 2) ** 0.5
                if d > radius:
                    radius = d
    if billboard:
        # Pivot at the sprite BASE (min y = ground), NOT the bbox centre: the
        # renderer's billboard rotation tilts with camera pitch, so a tall card
        # pivoted at mid-height swings its base off the ground ("floating trees").
        # Anchoring at the base keeps the trunk planted while the card faces the
        # camera. x/z stay centred so it spins about its own trunk.
        sub_x, sub_y, sub_z = bcx, miny, bcz                 # verts -> base-local
        orx, ory, orz = bcx * 256.0, miny * 256.0, bcz * 256.0
        btag = 1
    else:
        sub_x = sub_y = sub_z = 0.0                          # verts stay absolute
        # Verts are already absolute (the caller folds the mesh origin into them),
        # so the output mesh origin is 0 — otherwise the renderer's
        # load_translation(origin/256) would double-count the placement.
        orx, ory, orz = 0.0, 0.0, 0.0
        btag = 0
    struct.pack_into(MESH_HDR_FMT, buf, 0,
                     0x03, 0, btag, ncmd, n,
                     radius, bcx, bcy, bcz, orx, ory, orz,
                     0, out_cmds, out_verts, out_norms)
    w = 0
    for ci, (tp, lst) in enumerate(cmds):
        struct.pack_into("<hhiHHI", buf, out_cmds + ci * TD5_CSTRIDE,
                         0, tp, 0, len(lst), 0, 0)
        for p3, (nx, ny, nz) in lst:
            for s in range(3):
                px, py, pz, tu, tv = p3[s]
                vo = out_verts + w * TD5_VSTRIDE
                struct.pack_into("<fff fff I ff ff", buf, vo,
                                 px - sub_x, py - sub_y, pz - sub_z,
                                 0.0, 0.0, 0.0, 0xFF, tu, tv, 0.0, 0.0)
                no = out_norms + w * TD5_NSTRIDE
                struct.pack_into("<fffi", buf, no, nx, ny, nz, 1)
                w += 1
    return bytes(buf)


def _deindex_mesh(blob, moff, tree_pages=frozenset(),
                  banner_pages=frozenset(), route_fwd=None):
    """De-index one TD6 0x04 mesh at blob[moff] into a LIST of TD5 0x03 meshes.
    Returns (list_of_bytes, pages_used) or (None, reason). Large chunk meshes are
    split into spatial sub-meshes with tight bounding spheres (see below).
    tree_pages: set of texture-page indices whose meshes should be flagged as
    camera-facing billboards.
    banner_pages + route_fwd: when both given, drops the REAR face of any
    double-sided checkpoint/start/finish banner (see _banner_pages) so the port's
    cull-less renderer doesn't show it doubled/mirrored."""
    if moff + 0x38 > len(blob):
        return None, "hdr-oob"
    rt = blob[moff]
    tex_page = blob[moff + 1]
    if rt != 0x04:
        return None, f"rt=0x{rt:02x}"     # only de-index indexed meshes
    cmd_count = _i32(blob, moff + 0x04)
    vsrc_count = _i32(blob, moff + 0x08)
    radius = _f32(blob, moff + 0x0C)
    cx, cy, cz = _f32(blob, moff + 0x10), _f32(blob, moff + 0x14), _f32(blob, moff + 0x18)
    ox, oy, oz = _f32(blob, moff + 0x1C), _f32(blob, moff + 0x20), _f32(blob, moff + 0x24)
    cmds_off = moff + _u32(blob, moff + 0x2C)
    verts_off = moff + _u32(blob, moff + 0x30)
    if not (1 <= cmd_count <= 256) or not (0 < vsrc_count <= 65535):
        return None, "count"
    if verts_off + vsrc_count * TD6_VSTRIDE > len(blob):
        return None, "verts-oob"
    if cmds_off + cmd_count * TD6_CSTRIDE > len(blob):
        return None, "cmds-oob"

    # Gather triangle corner indices across every 32-byte command. index_off is
    # mesh-relative. Be tolerant: stop at the first malformed command and keep
    # whatever valid commands preceded it (don't discard the whole mesh).
    # Per-command: keep each TD6 command's own texture page (i16 @cmd+2). The
    # TD5 renderer walks commands with vertex_data_ptr=0 via a running cursor
    # (td5_render.c:1870), so emit ONE TD5 command per TD6 command and lay the
    # expanded verts in command order. Stop at the first malformed command.
    # CRITICAL: each 32-byte command has its OWN vertex buffer (vert_off @cmd+0x10,
    # vert_count @cmd+8) and its indices are LOCAL (0-based) to that buffer. The
    # mesh-header verts_off is only command 0's buffer. Using it for every command
    # pulls cmd1+ vertices from the wrong place -> spike/glitch geometry.
    cmd_records = []     # (tex_page, cmd_voff, [local corner indices])
    for c in range(cmd_count):
        cmd = cmds_off + c * TD6_CSTRIDE
        if cmd + TD6_CSTRIDE > len(blob):
            break
        tp = struct.unpack_from("<h", blob, cmd + 2)[0] & 0xFFFF
        cmd_vcnt = _i32(blob, cmd + 0x08)
        idx_count = _i32(blob, cmd + 0x0C)
        cmd_voff = moff + _i32(blob, cmd + 0x10)
        idx_off = _i32(blob, cmd + 0x14)
        idx_base = moff + idx_off
        if idx_count <= 0 or idx_count % 3 != 0:
            break
        if cmd_vcnt <= 0 or cmd_voff < 0 or cmd_voff + cmd_vcnt * TD6_VSTRIDE > len(blob):
            break
        if idx_base < 0 or idx_base + idx_count * 2 > len(blob):
            break
        tmp = []
        bad = False
        for k in range(idx_count):
            vi = struct.unpack_from("<H", blob, idx_base + k * 2)[0]
            if vi >= cmd_vcnt:                  # indices are local to this command
                bad = True
                break
            tmp.append(vi)
        if bad:
            break
        cmd_records.append((tp, cmd_voff, tmp))
    n = sum(len(cc) for _, _, cc in cmd_records)
    if n == 0 or n % 3 != 0 or n > 200000:
        return None, "no-valid-cmds"

    # [PROP/BILLBOARD ORIGIN FOLD] TD6 instanced PROP meshes (trees, palms,
    # statues, people, lamps, flags, trunks, ...) store their WORLD placement in
    # the mesh ORIGIN (ox,oy,oz @ +0x1C, 24.8 fixed-point) and keep their
    # vertices LOCAL to that origin; solid geometry (road/buildings/walls) has
    # origin=(0,0,0) and absolute verts. Verified across all 11 migrated TD6
    # levels: NO mesh has a non-zero origin AND absolute (large) verts, so adding
    # origin/256 to every vertex is a NO-OP for origin=0 meshes and the correct
    # world placement for the props. Without it every prop collapses onto the
    # world origin (London's whole tree set piled near span 110 -> the "tree in
    # the middle of the road" at the start). _pack_mesh then sets the output mesh
    # origin to 0 (verts are now absolute) so the renderer's
    # load_translation(origin/256) doesn't double-count.
    fold_x, fold_y, fold_z = ox / 256.0, oy / 256.0, oz / 256.0

    # Read source positions+uv from a command's OWN vertex buffer.
    def vpos(cmd_voff, vi):
        sv = cmd_voff + vi * TD6_VSTRIDE
        return (_f32(blob, sv) + fold_x, _f32(blob, sv + 4) + fold_y,
                _f32(blob, sv + 8) + fold_z,
                _f32(blob, sv + 0x18), _f32(blob, sv + 0x1C))

    pages_used = sorted({tp for tp, _, _ in cmd_records})
    # Flatten to a triangle list with COMPUTED face normals oriented to the
    # upper hemisphere (TD6 track winding is unknown, so an inward normal would
    # light it from behind -> flat-dark; ny>=0 lets ground/roofs catch the
    # top-down daylight while walls keep their horizontal side-light component).
    tris = []   # (tex_page, [(px,py,pz,tu,tv) x3], (nx,ny,nz))
    for tp, cmd_voff, corners in cmd_records:
        for t in range(0, len(corners), 3):
            p = [vpos(cmd_voff, corners[t + s]) for s in range(3)]
            ux, uy, uz = p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]
            wx, wy, wz = p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]
            nx, ny, nz = uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx
            ln = (nx * nx + ny * ny + nz * nz) ** 0.5
            if ln > 1e-6:
                nx, ny, nz = nx / ln, ny / ln, nz / ln
            else:
                nx, ny, nz = 0.0, 1.0, 0.0
            if ny < 0.0:
                nx, ny, nz = -nx, -ny, -nz
            tris.append((tp, p, (nx, ny, nz)))

    # [BANNER REAR-FACE CULL] The numbered checkpoint / start / finish gantry
    # signs are DOUBLE-SIDED in TD6. The port renderer runs CullMode=NONE (its
    # Y-negated projection makes winding-based culling unreliable), so the rear
    # face — backface-culled in TD6 — shows through as a doubled/mirrored banner.
    # Drop one face, keeping the readable front.
    #
    # WHICH face is readable: determined empirically (London START banner,
    # user-verified) and confirmed uniform across all 5 P2P tracks (every banner
    # face shares the same UV handedness). TD6 winds banner faces OPPOSITE to the
    # standard outward-normal convention: the readable front's geometric (winding)
    # normal points ALONG the route (forward, dot > 0); the mirrored rear points
    # back toward the car (dot < 0). So keep dot > 0, drop dot < 0. Guard: only
    # cull when BOTH faces are present, so a one-sided sign can never vanish.
    if banner_pages and route_fwd is not None:
        bidx = [k for k, (tp, _, _) in enumerate(tris) if tp in banner_pages]
        if bidx:
            fwd_dot = {}
            for k in bidx:
                _tp, p3, _ = tris[k]
                ux, uy, uz = (p3[1][0] - p3[0][0], p3[1][1] - p3[0][1], p3[1][2] - p3[0][2])
                wx, wy, wz = (p3[2][0] - p3[0][0], p3[2][1] - p3[0][1], p3[2][2] - p3[0][2])
                gnx = uy * wz - uz * wy          # geometric normal (pre-reorient)
                gnz = ux * wy - uy * wx
                cx = (p3[0][0] + p3[1][0] + p3[2][0]) / 3.0
                cz = (p3[0][2] + p3[1][2] + p3[2][2]) / 3.0
                fx, fz = route_fwd(cx, cz)
                fwd_dot[k] = gnx * fx + gnz * fz
            front = [k for k in bidx if fwd_dot[k] > 0.0]   # readable -> keep
            rear  = [k for k in bidx if fwd_dot[k] < 0.0]   # mirrored rear -> drop
            if front and rear:                              # genuinely double-sided
                drop = set(rear)
                tris = [t for k, t in enumerate(tris) if k not in drop]

    # Separate camera-facing TREE sprites (tree-page triangles) from SOLID
    # geometry (road/buildings/walls). A TD6 chunk mesh mixes both, and the TD5
    # billboard tag is per-MESH (it rotates the WHOLE mesh to face the camera),
    # so if one mesh carried a tree page the road+walls inside it billboard too
    # and "swim" as you drive. Emitting them as distinct sub-meshes keeps solid
    # geometry world-fixed and lets only the tree sprites face the camera.
    solid = [t for t in tris if t[0] not in tree_pages]
    treet = [t for t in tris if t[0] in tree_pages]

    def _bucket(items, cell):
        """xz-grid bucket -> list of triangle lists (one TIGHT sub-mesh each)."""
        ext_x = (max(v[0] for _, p3, _ in items for v in p3) -
                 min(v[0] for _, p3, _ in items for v in p3))
        ext_z = (max(v[2] for _, p3, _ in items for v in p3) -
                 min(v[2] for _, p3, _ in items for v in p3))
        if ext_x < cell and ext_z < cell:
            return [items]
        cells = {}
        for tri in items:
            _, p3, _ = tri
            mx = (p3[0][0] + p3[1][0] + p3[2][0]) / 3.0
            mz = (p3[0][2] + p3[1][2] + p3[2][2]) / 3.0
            cells.setdefault((int(mx // cell), int(mz // cell)), []).append(tri)
        return list(cells.values())

    # SPLIT large chunk meshes into spatial sub-meshes with TIGHT bounding
    # spheres. TD6 ships giant chunks (one can span ~24% of the track); a single
    # loose sphere keeps the whole chunk alive until its CENTRE leaves the
    # frustum, so big blocks pop in/out as you drive. xz-grid buckets cull
    # precisely, matching native TD5's fine per-span meshes.
    out = []
    if solid:
        for b in _bucket(solid, 6000.0):
            mb = _pack_mesh(b, False, ox, oy, oz)
            if mb:
                out.append(mb)
    if treet:
        # The tree-page triangles in ONE TD6 mesh form a SINGLE tree CARD — a
        # wide quad (~6000 units) with the full tree texture (UV 0-1, trunk at the
        # bottom). Do NOT cluster/split it: a sub-card-sized split tears the card
        # into fragments that each billboard about their own pivot and smear into
        # a foliage wall. Billboard the whole card as ONE camera-facing sprite,
        # anchored at its base so the trunk stays planted.
        # Per-TRIANGLE split only by orientation: VERTICAL faces (ny~0) are tree
        # cards -> billboard; UP-facing faces (ny large) are ground/hedge surfaces
        # -> world-fixed solid (billboarding a surface would lift it off the ground).
        vert_tris = [t for t in treet if abs(t[2][1]) < 0.55]
        flat_tris = [t for t in treet if abs(t[2][1]) >= 0.55]
        if vert_tris:
            xs = [v[0] for _, p3, _ in vert_tris for v in p3]
            ys = [v[1] for _, p3, _ in vert_tris for v in p3]
            zs = [v[2] for _, p3, _ in vert_tris for v in p3]
            w = max(max(xs) - min(xs), max(zs) - min(zs))
            h = max(ys) - min(ys)
            if w > 1.8 * h:
                # A WIDE vertical strip is a fixed tree-LINE / hedge, not a single
                # tree card. Billboarding it swings the whole line to face the
                # camera as you drive (visibly wrong). Render it world-fixed solid.
                for b in _bucket(vert_tris, 6000.0):
                    mb = _pack_mesh(b, False, ox, oy, oz)
                    if mb:
                        out.append(mb)
            else:
                # Single upright tree card -> camera-facing billboard.
                mb = _pack_mesh(vert_tris, True, ox, oy, oz)
                if mb:
                    out.append(mb)
        if flat_tris:
            for b in _bucket(flat_tris, 6000.0):              # world-fixed surface
                mb = _pack_mesh(b, False, ox, oy, oz)
                if mb:
                    out.append(mb)
    if not out:
        return None, "no-valid-cmds"
    return out, pages_used


def convert_models(td6_models: bytes, verbose=True, tree_pages=frozenset(),
                   banner_pages=frozenset(), route_fwd=None):
    """Rewrite a TD6 models.dat into a TD5-native models.dat (format A).
    tree_pages: texture-page indices to flag as camera-facing billboards.
    banner_pages + route_fwd: drop the rear face of double-sided checkpoint/
    start/finish banners (see _deindex_mesh)."""
    out_blocks = []          # list of (entry_index, block_bytes or None)
    stats = {"meshes": 0, "deindexed": 0, "skipped": {}, "entries": 0, "blocks": 0}
    entries = list(_iter_blocks(td6_models))
    n_entries = (entries[-1][0] + 1) if entries else 0
    used_pages = set()
    slot = {}
    for ei, meshes in entries:
        stats["entries"] += 1
        if not meshes:
            continue
        td5_meshes = []
        for moff in meshes:
            if moff is None:
                td5_meshes.append(None); continue
            stats["meshes"] += 1
            mblist, info = _deindex_mesh(td6_models, moff, tree_pages,
                                         banner_pages, route_fwd)
            if mblist is None:
                stats["skipped"][info] = stats["skipped"].get(info, 0) + 1
                td5_meshes.append(None)
            else:
                stats["deindexed"] += 1
                stats["submeshes"] = stats.get("submeshes", 0) + len(mblist)
                used_pages.update(info)         # info = list of tex pages on success
                td5_meshes.extend(mblist)       # split mesh -> >=1 sub-meshes
        if not any(m is not None for m in td5_meshes):
            continue
        # The renderer (td5_render_span_display_list) rejects a block whose
        # sub_count exceeds 256. Splitting keeps cells coarse enough that this is
        # never hit in practice, but cap defensively so an over-split block
        # degrades to "drop the tail" rather than "drop the whole block".
        if len(td5_meshes) > 256:
            stats["capped_blocks"] = stats.get("capped_blocks", 0) + 1
            td5_meshes = td5_meshes[:256]
        # Build a TD5 display-list block: [sub_count][mesh_off..][mesh data..]
        sub = len(td5_meshes)
        if sub > stats.get("max_sub", 0):
            stats["max_sub"] = sub
        body = bytearray()
        offs = []
        data = bytearray()
        hdr_sz = 4 + sub * 4
        for mb in td5_meshes:
            if mb is None:
                offs.append(0)
            else:
                offs.append(hdr_sz + len(data))
                data += mb
        block = bytearray()
        block += struct.pack("<I", sub)
        for o in offs:
            block += struct.pack("<I", o)
        block += data
        slot[ei] = bytes(block)
        stats["blocks"] += 1

    # Assemble format-A container: [count][ (offset,size)*count ][ dummy ][ blocks ].
    # The loader's format detection keys off DWORD[1] (entry[0]'s offset field): it
    # must be a valid block offset, never 0. We emit a 4-byte dummy empty block
    # (sub_count=0) at offset table_sz and point every EMPTY entry there, so even if
    # entry 0 is empty the file still parses (strict format-A: dword1==4+count*8).
    count = n_entries
    table_sz = 4 + count * 8
    table = bytearray(count * 8)
    body = bytearray()
    dummy_off = table_sz
    body += struct.pack("<I", 0)                 # dummy empty block (sub_count=0)
    for ei in range(count):
        blk = slot.get(ei)
        if blk is None:
            struct.pack_into("<II", table, ei * 8, dummy_off, 4)
        else:
            blk_off = table_sz + len(body)
            struct.pack_into("<II", table, ei * 8, blk_off, len(blk))
            body += blk
    blob = bytearray()
    blob += struct.pack("<I", count)
    blob += table
    blob += body
    stats["used_pages"] = sorted(used_pages)
    if verbose:
        print(f"  models.dat: {stats['entries']} entries, {stats['blocks']} blocks, "
              f"{stats['deindexed']}/{stats['meshes']} meshes de-indexed "
              f"-> {stats.get('submeshes', 0)} sub-meshes (max {stats.get('max_sub', 0)}/block"
              f"{', capped ' + str(stats['capped_blocks']) if stats.get('capped_blocks') else ''}); "
              f"skipped={stats['skipped']}; tex_pages={len(used_pages)} "
              f"(max {max(used_pages) if used_pages else -1}); out={len(blob)} bytes")
    return bytes(blob), stats


# ============================================================ textures.dat (Milestone B)
# Build a TD5 TEXTURES.DAT (64x64 8-bit palettized, BGR palette) from TD6's
# externalized LEVELS/texture<N>.zip + its 64-byte-record textures.dir. The mesh
# command's texture_page_id is the INDEX into textures.dir. TD5 format
# (td5_asset.c:2324): u32 page_count, u32 offsets[page_count]; each page =
# byte[3] pad + byte type + i32 pal_count + BGR[pal_count*3] + u8[4096] indices.
def _td6_texture_zip(td6_level: int) -> str:
    return os.path.join(TD6_LEVELS, f"texture{td6_level}.zip")


def _parse_textures_dir(zp):
    data = read_zip_entry(zp, "textures.dir")
    if not data:
        return []
    out = []
    for o in range(0, len(data) - 63, 64):
        name = data[o:o + 32].split(b"\x00")[0].decode("latin1", "replace")
        out.append(name)
    return out


def _billboard_pages(names):
    """Texture pages whose meshes should billboard (camera-facing point sprites).

    TD6 point trees ('1tree','2tree','t1tree1',...) are flat quads that must
    face the camera. But TREE WALLS ('1TreeWa'/'1TreeWb') and similar are large
    FIXED backdrops — billboarding a tall wall makes it spin to face you as you
    drive, so exclude wall/backdrop names. The mesh that carries a non-billboard
    tree page is emitted as ordinary world-fixed geometry."""
    out = set()
    for i, n in enumerate(names):
        nl = n.lower()
        if "tree" not in nl:
            continue
        if "treew" in nl or "wall" in nl or "backd" in nl or "fence" in nl:
            continue
        out.add(i)
    return frozenset(out)


def _banner_pages(names):
    """Texture pages of the numbered checkpoint / START / FINISH gantry banners.

    Each migrated TD6 city names them differently (visually confirmed — see
    re/tools/extract_td6_checkpoints.py): Paris/HongKong spelled `1one..1four`,
    NewYork bare digits `1..4`, Rome `Check01a..Check05a`, London `Kstage1..4`,
    plus the per-city `start`/`finish` banners. These thin road-spanning signs
    are DOUBLE-SIDED in TD6; the TD5 port renderer can't backface-cull
    (CullMode=NONE — its Y-negated projection makes winding-based culling
    unreliable), so the rear face TD6 culled shows through as a doubled /
    mirrored banner. _deindex_mesh drops that rear face for these pages."""
    import re
    out = set()
    for i, n in enumerate(names):
        b = n.rsplit(".", 1)[0].lower()
        if (re.match(r"^\d*(one|two|three|four|five)$", b) or  # Paris / HongKong
                re.match(r"^[1-9]$", b) or                      # New York digits
                re.match(r"^check0?[1-9][ab]?$", b) or          # Rome Check0N
                re.match(r"^kstage[1-9]$", b) or                # London Kstage
                re.match(r"^k?\d?(start|finish)\d?$", b)):      # start / finish
            out.add(i)
    return frozenset(out)


def make_route_fwd(strip, circuit):
    """Return f(x,z) -> (fx,fz): the route's forward (increasing-span) tangent at
    the strip span nearest world point (x,z). Used to tell a double-sided
    banner's REAR face (geometric normal points forward, away from the
    approaching car) from its FRONT face (points back toward the car)."""
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)

    def vtx(vi):
        x, y, z = struct.unpack_from("<hhh", strip, vtx_off + vi * VERT_STRIDE)
        return x, z

    centers = []
    for i in range(span_cnt):
        o = span_off + i * SPAN_STRIDE
        lv, rv = struct.unpack_from("<HH", strip, o + 4)
        ox, oy, oz = struct.unpack_from("<iii", strip, o + 12)
        try:
            lx, lz = vtx(lv); rx, rz = vtx(rv)
        except struct.error:
            lx = lz = rx = rz = 0
        centers.append((ox + (lx + rx) / 2.0, oz + (lz + rz) / 2.0))

    def fwd(x, z):
        bi, bd = 0, 1e30
        for i, (cx, cz) in enumerate(centers):
            d = (cx - x) ** 2 + (cz - z) ** 2
            if d < bd:
                bd, bi = d, i
        if circuit:
            a = centers[(bi - 2) % span_cnt]; b = centers[(bi + 2) % span_cnt]
        else:
            a = centers[max(0, bi - 2)]; b = centers[min(span_cnt - 1, bi + 2)]
        return (b[0] - a[0], b[1] - a[1])

    return fwd


def _build_page_64(img):
    """Quantize a PIL image to a TD5 64x64 8-bit page. RGBA images with
    transparent pixels become ALPHA-KEYED pages (type 1, palette index 0 =
    transparent, which TD5 renders as alpha=0); opaque images become type 0.
    Returns (page_bytes, page_type)."""
    from PIL import Image
    img = img.resize((64, 64), Image.BILINEAR)
    keyed = False
    alpha = None
    if img.mode in ("RGBA", "LA", "PA"):
        alpha = img.convert("RGBA").getchannel("A")
        keyed = alpha.getextrema()[0] < 128
    rgb = img.convert("RGB")
    if keyed:
        # Reserve palette index 0 for transparent; quantise colours into 1..255.
        q = rgb.quantize(colors=255)
        idx = bytearray(((p + 1) & 0xFF) for p in q.getdata())
        for i, a in enumerate(alpha.getdata()):
            if a < 128:
                idx[i] = 0
        pal = (q.getpalette() or []) + [0] * (255 * 3)
        bgr = bytearray((0, 0, 0))                 # index 0 = transparent
        for i in range(255):
            r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
            bgr += bytes((b, g, r))
        ptype = 1
    else:
        q = rgb.quantize(colors=256)
        idx = bytearray(q.getdata())
        pal = (q.getpalette() or []) + [0] * (256 * 3)
        bgr = bytearray()
        for i in range(256):
            r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
            bgr += bytes((b, g, r))
        ptype = 0
    page = bytearray()
    page += bytes((0, 0, 0, ptype))               # byte[3] pad + type
    page += struct.pack("<i", 256)                # pal_count (always 256 slots)
    page += (bytes(bgr) + bytes(768))[:768]       # 256*3 BGR
    page += (bytes(idx) + bytes(4096))[:4096]     # 64x64 indices
    return bytes(page), ptype


def build_textures_dat(td6_level: int, page_count: int, verbose=True):
    """page_count pages, indexed 0..page_count-1 == textures.dir index == mesh tex_page."""
    from PIL import Image
    zp = _td6_texture_zip(td6_level)
    names = _parse_textures_dir(zp)
    if not names:
        if verbose:
            print(f"  textures.dat: no textures.dir in {zp} — skipping (meshes stay untextured)")
        return None
    pages = []
    missing = 0
    keyed = 0
    for pg in range(page_count):
        img = None
        if pg < len(names):
            tga = read_zip_entry(zp, names[pg])
            if tga:
                try:
                    img = Image.open(io.BytesIO(tga))
                    img.load()
                except Exception:
                    img = None
        if img is None:
            missing += 1
            img = Image.new("RGB", (64, 64), (96, 96, 96))   # neutral grey placeholder
        page, ptype = _build_page_64(img)
        if ptype == 1:
            keyed += 1
        pages.append(page)
    # assemble: u32 page_count, u32 offsets[], pages
    hdr_sz = 4 + page_count * 4
    body = bytearray()
    offs = []
    for p in pages:
        offs.append(hdr_sz + len(body))
        body += p
    out = bytearray()
    out += struct.pack("<I", page_count)
    for o in offs:
        out += struct.pack("<I", o)
    out += body
    if verbose:
        print(f"  textures.dat: {page_count} pages from {os.path.basename(zp)} "
              f"({len(names)} dir entries, {keyed} alpha-keyed, {missing} grey); out={len(out)} bytes")
    return bytes(out)


def used_pages_of(td6_models: bytes):
    """Set of texture-page ids referenced by a TD6 models.dat (command tex_page
    @cmd+2). Fast: parses command headers only, no vertex de-index. Mirrors the
    used_pages convert_models accumulates, so build_loose_textures can run
    standalone without re-de-indexing every mesh."""
    used = set()
    for ei, mlist in _iter_blocks(td6_models):
        for moff in mlist:
            if moff is None:
                continue
            cc = _i32(td6_models, moff + 4)
            cmds_off = moff + _u32(td6_models, moff + 0x2C)
            for c in range(cc):
                cmd = cmds_off + c * TD6_CSTRIDE
                if cmd + TD6_CSTRIDE > len(td6_models):
                    break
                ic = _i32(td6_models, cmd + 0x0C)
                if ic <= 0 or ic % 3 != 0:
                    break
                tp = struct.unpack_from("<h", td6_models, cmd + 2)[0] & 0xFFFF
                used.add(tp)
    return used


def build_loose_textures(td6_level: int, page_count: int, od: str, verbose=True):
    """Emit NATIVE-RESOLUTION loose PNGs (textures/tex_NNN.png) for every texture
    page, so the engine loads them at full size (128x128 / 256x256) with real
    alpha instead of the 64x64 8-bit palettized TEXTURES.DAT page. The TD6
    textures.dir records carry each texture's native W,H (u16 @ +52,+56) and the
    TGAs are 24-bit RGB or 32-bit RGBA; downsizing to 64x64 was the dominant
    cause of blurry/unreadable city textures. Page index == textures.dir index ==
    mesh command tex_page == TEXTURES.DAT page, so tex_NNN.png lines up with the
    engine's per-page upload. TEXTURES.DAT is still written for the per-page
    transparency/blend type byte and as a fallback."""
    from PIL import Image
    zp = _td6_texture_zip(td6_level)
    names = _parse_textures_dir(zp)
    if not names:
        return 0
    tdir = os.path.join(od, "textures")
    os.makedirs(tdir, exist_ok=True)
    written = 0
    for pg in range(page_count):
        if pg >= len(names):
            continue
        tga = read_zip_entry(zp, names[pg])
        if not tga:
            continue
        try:
            img = Image.open(io.BytesIO(tga))
            img.load()
        except Exception:
            continue
        # Keep real alpha only when the TGA actually has transparency (some have
        # an all-0/all-255 junk alpha); otherwise emit opaque RGB so a junk
        # channel doesn't make the whole surface invisible.
        out = img.convert("RGB")
        if img.mode in ("RGBA", "LA", "PA"):
            a = img.convert("RGBA").getchannel("A")
            lo, hi = a.getextrema()
            if lo < 128 <= hi:
                out = img.convert("RGBA")
        out.save(os.path.join(tdir, f"tex_{pg:03d}.png"))
        written += 1
    if verbose:
        print(f"  loose textures: wrote {written} native-res PNGs -> {tdir}")
    return written


# ----------------------------------------------------------------------- sky
def build_sky(td6_level: int, od: str, verbose=True):
    """Convert TD6 per-level sky tiles (sky.zip level<N>A/B.tga) to the
    FORWSKY.png / BACKSKY.png the port loads (256x256 RGB)."""
    from PIL import Image
    sky_zip = os.path.join(TD6_DIR, "sky.zip")

    def conv(tga_name, png_name):
        data = read_zip_entry(sky_zip, tga_name)
        if not data:
            return False
        try:
            im = Image.open(io.BytesIO(data)).convert("RGB").resize((256, 256))
            im.save(os.path.join(od, png_name))
            return True
        except Exception:
            return False

    a = conv(f"level{td6_level}A.tga", "FORWSKY.png")
    b = conv(f"level{td6_level}B.tga", "BACKSKY.png")
    if a and not b:
        b = conv(f"level{td6_level}A.tga", "BACKSKY.png")
    if verbose:
        print(f"  sky: FORWSKY={a} BACKSKY={b} (sky.zip level{td6_level}A/B)")
    return a or b


# ============================================================ routes (LEFT/RIGHT.TRK)
# TD5 AI route tables are 3 bytes per main-ring span:
#   [+0] lane byte  (~128 = lane centre; used by ComputeSignedTrackOffset)
#   [+1] heading byte: route_heading_12bit = (b * 0x102C) >> 8, and the heading
#        is atan2(dx, dz) in 12-bit units (0x1000 = full circle) along the lane
#        centreline -> b = round(heading_12 * 256 / 0x102C). [decoded from Moscow]
#   [+2] 255 (validity/■ sentinel; 0..3 means "junction zone, skip")
# left.trk == right.trk for the centreline. Without these the AI steers off the
# TD6 track at curves (drives out of bounds); with them it follows the racing line.
def build_routes(strip: bytes, circuit: bool = True):
    """Synthesize the AI corridor as DISTINCT left + right route tables.

    The AI builds its lateral steering corridor from the LEFT route lane byte
    and the RIGHT route lane byte (td5_track_compute_signed_offset, per
    UpdateRaceActors). The lane byte is a 0..255 position ACROSS the strip width
    (0=left rail, 255=right rail, 128=centre; SampleTrackTargetPoint interpolates
    by it). The old converter wrote left.trk == right.trk == 128 → a ZERO-WIDTH
    corridor pinned to centre, so the AI had no road bounds and the cars spread
    across the full 8-lane strip into weird outer sub-lanes / onto grass.

    Now emit a real corridor matching the PAVED ROAD (centre lanes, excluding the
    SHOULDER_FRAC grass lanes each side — same split as remap_surface):
      LEFT.TRK  lane byte = left road edge  = round(sh/L * 255)
      RIGHT.TRK lane byte = right road edge = round((L-sh)/L * 255)
    Heading byte is the track forward direction from the true span CENTRE (avg of
    the 4 quad corners), shared by both. Returns (left_table, right_table)."""
    import math
    span_off, span_cnt, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)

    def vtx(vi):
        x, y, z = struct.unpack_from("<hhh", strip, vtx_off + vi * VERT_STRIDE)
        return x, z

    def span_fields(i):
        o = span_off + i * SPAN_STRIDE
        lv, rv = struct.unpack_from("<HH", strip, o + 4)
        lanes = strip[o + 3] & 0x0F
        ox, oy, oz = struct.unpack_from("<iii", strip, o + 12)
        return lv, rv, lanes, ox, oz

    def center(i):
        # Left/right RAIL-vertex midpoint (+ origin) — same clean center the
        # preview tool uses. The previous 4-corner average (lv,lv+lanes,rv,
        # rv+lanes) pulled in jittery far-edge vertices and made even straight
        # sections wobble in heading.
        lv, rv, lanes, ox, oz = span_fields(i)
        lx, lz = vtx(lv)
        rx, rz = vtx(rv)
        return (ox + (lx + rx) / 2.0, oz + (lz + rz) / 2.0)

    left = bytearray(span_cnt * 3)
    right = bytearray(span_cnt * 3)
    # Heading byte per span. Use a CENTERED multi-span window (not a single
    # center(s)->center(s+1) step): junction spans (types 8-11) and local
    # geometry jitter give wildly wrong single-step directions (e.g. London span
    # 340 came out ~90deg off its straight), which then corrupt BOTH the spawn
    # yaw (via td5_ai_correct_spawn_heading) and the AI corridor (cars weave into
    # walls). Averaging the tangent over +/-K spans skips the bad single spans;
    # on a straight every span shares one heading, on a corner it lags slightly.
    K = 3

    def heading_byte(s):
        if circuit:
            ax, az = center((s - K) % span_cnt)
            bx, bz = center((s + K) % span_cnt)
        else:
            lo = s - K if s >= K else 0
            hi = s + K if s + K <= span_cnt - 1 else span_cnt - 1
            if hi == lo:
                return 4
            ax, az = center(lo)
            bx, bz = center(hi)
        dx, dz = bx - ax, bz - az
        if dx == 0 and dz == 0:
            return 4
        h12 = int(round((math.atan2(dx, dz) / (2 * math.pi)) * 4096)) & 0xFFF
        return max(4, min(253, int(round(h12 * 256 / 0x102C))))

    for s in range(span_cnt):
        b = heading_byte(s)
        _lv, _rv, lanes, _ox, _oz = span_fields(s)
        L = lanes if lanes >= 1 else 1
        sh = 0 if lanes <= 2 else max(1, int(round(lanes * SHOULDER_FRAC)))
        # Trace-confirmed: the AI tracks the LEFT route's lateral position as its
        # racing line. Put LEFT at the ROAD CENTRE (byte 128 = centre of the
        # symmetric road) so the AI drives the middle of the paved road, not the
        # left edge. RIGHT = right road edge — a DISTINCT bound (avoids the
        # zero-width corridor that made the single-centreline route jitter).
        left_byte = 128
        right_byte = max(left_byte + 8, min(255, int(round((L - sh) / L * 255))))
        left[s * 3 + 0] = left_byte
        left[s * 3 + 1] = b
        left[s * 3 + 2] = 255
        right[s * 3 + 0] = right_byte
        right[s * 3 + 1] = b
        right[s * 3 + 2] = 255
    return bytes(left), bytes(right)


# ----------------------------------------------------------------- levelinf synth
def synth_levelinf(circuit: int) -> bytes:
    """TD5 LEVELINF.DAT is 100 bytes; the engine reads only DWORD[0] = circuit flag
    (1=circuit/laps, 0=point-to-point). td5_asset.c:2025-2035. The remaining 96 bytes
    are environment config the port treats as opaque (g_track_environment_config);
    zero-fill is safe for a driving proof."""
    buf = bytearray(100)
    struct.pack_into("<I", buf, 0, 1 if circuit else 0)
    return bytes(buf)


# ----------------------------------------------------------------- surface remap
def remap_surface(strip: bytes, shoulders: bool = True, road_only: bool = False):
    """Remap TD6's per-span surface index to TD5's surface_attribute encoding so
    grass actually slows the car.

    TD6 stores a FLAT surface-type index in span byte +0x01 (values 0..20 on
    level010, sequential — NOT TD5's nibble pairs like 0x11/0x31/0xF1). TD5
    instead splits +0x01 into low-nibble (on-road type 0-15) + high-nibble
    (off-road type), and selects per-lane via the +0x02 lane bitmask
    (surface_type_for_span_lane td5_track.c: flagged lane -> (attr>>4)|0x10).
    TD5's grip/drag tables (k_grip_004748C0 / k_drag_00474900) treat surface
    types 0-15 as ROAD (grip ~0x100, ~0 drag) and 16-31 as GRASS/off-road
    (grip 0xC0 + drag 0x20 = slowdown).

    Passed through raw, TD6's grass spans (idx>=16, e.g. the wide run-off fans at
    spans 333-356) are misread: low nibble gives road 0-4 and lane_bitmask=0
    means NO lane is ever off-road -> grass never applies -> cars don't slow on
    grass. Remap per span so the engine's computed surface type == TD6's index:
      idx <16 (road) : low nibble = idx,        bitmask = 0   (all lanes road idx)
      idx>=16 (grass): high nibble = idx-16,    bitmask = all-lanes (every lane
                       returns (idx-16)|0x10 == idx = grass)
    Branch/sentinel spans are remapped too (harmless for sentinels)."""
    span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", strip, 0)
    out = bytearray(strip)
    n_grass = 0
    n_shoulder = 0
    n_road_only = 0
    for i in range(total):
        o = span_off + i * SPAN_STRIDE
        T = out[o + 1]
        lanes = out[o + 3] & 0x0F
        if road_only:
            # URBAN tracks: TD6's surface index >=16 is heavily used for ROAD
            # (e.g. Hong Kong branch roads use idx 25-27), not grass — and the
            # per-track index->material meaning differs, so the >=16 grass rule
            # mis-slows real roads. The surface attribute is PHYSICS-ONLY (grip/
            # drag; textures come from the mesh), so flatten every span to road
            # type 0 (full grip, no off-road lanes). No false slowdown on streets;
            # appearance unchanged. (A genuine slow zone can be re-added per-track.)
            out[o + 1] = 0
            out[o + 2] = 0
            n_road_only += 1
            continue
        if T >= 16:
            # whole-span grass (TD6 run-off "fans"): every lane off-road.
            out[o + 1] = ((T - 16) & 0x0F) << 4
            out[o + 2] = ((1 << lanes) - 1) & 0xFF if lanes and lanes <= 8 else 0xFF
            n_grass += 1
        else:
            # road span: keep the road type in the low nibble, and add GRASS
            # SHOULDERS on the outer `sh` lanes each side (high nibble = grass
            # type 1 -> surface 17, grip 0xC0 + drag 0x20). TD6 stores no per-lane
            # grass (byte+2=0), but the paved road is the CENTRE of the wide strip
            # with grass verges the whole lap, so flag the outer lanes -> drifting
            # wide reads grass and slows. Road width = SHOULDER_FRAC of each side.
            sh = 0 if (not shoulders or lanes <= 2) else max(1, int(round(lanes * SHOULDER_FRAC)))
            out[o + 1] = (1 << 4) | (T & 0x0F)
            mask = 0
            for k in range(sh):
                mask |= (1 << k)
            for k in range(lanes - sh, lanes):
                mask |= (1 << k)
            out[o + 2] = mask & 0xFF
            if mask:
                n_shoulder += 1
    return bytes(out), n_grass, n_shoulder


# Fraction of each side of a road span treated as grass shoulder (tunable; the
# paved road is the centre 1-2*SHOULDER_FRAC of the strip width). 0.25 -> road
# is the centre ~half. Lower = wider road / thinner verges.
SHOULDER_FRAC = 0.25


# --------------------------------------------------------------------- milestone A
def convert_milestone_a(td6_level: int, td5_level: int, circuit: int,
                        with_models=True, verbose=True, shoulders=True,
                        road_only=False):
    zp = td6_level_zip(td6_level)
    if not os.path.exists(zp):
        print(f"ERROR: TD6 level zip not found: {zp}", file=sys.stderr)
        return False

    strip = read_zip_entry(zp, "strip.dat")
    stripb = read_zip_entry(zp, "stripb.dat")
    if not strip:
        print(f"ERROR: no strip.dat in {zp}", file=sys.stderr)
        return False

    od = out_dir(td5_level)
    os.makedirs(od, exist_ok=True)

    def write(name, data):
        if data is None:
            return
        with open(os.path.join(od, name), "wb") as f:
            f.write(data)
        if verbose:
            print(f"  wrote {name:14} {len(data):8} bytes")

    strip, n_grass, n_shoulder = remap_surface(strip, shoulders=shoulders, road_only=road_only)
    if verbose:
        if road_only:
            print(f"  surface remap: ROAD-ONLY (urban) — all spans -> road, no grass/shoulders")
        else:
            print(f"  surface remap: {n_grass} whole-grass spans + {n_shoulder} road spans w/ grass shoulders")
    write("STRIP.DAT", strip)
    if stripb:
        stripb, n_grass_b, n_shoulder_b = remap_surface(stripb, shoulders=shoulders, road_only=road_only)
        if verbose:
            print(f"  surface remap (STRIPB): {n_grass_b} whole-grass + {n_shoulder_b} shoulders")
    write("STRIPB.DAT", stripb)
    write("LEVELINF.DAT", synth_levelinf(circuit))
    # AI corridor: DISTINCT left/right road edges (not a single centreline) so the
    # AI has real lateral bounds = the paved road, and stops wandering into the
    # grass-shoulder sub-lanes. build_routes returns (left, right).
    left_routes, right_routes = build_routes(strip, circuit=bool(circuit))
    write("LEFT.TRK", left_routes)
    write("RIGHT.TRK", right_routes)

    # Reverse AI corridor (LEFTB/RIGHTB.TRK), built from the REVERSE strip exactly
    # like the forward routes are built from STRIP.DAT. The engine selects the
    # "B"-suffixed route files when reverse direction is active (LoadTrackRuntimeData
    # @0x42fb90 indexes a filename table by the reverse flag); without these a
    # reverse race has no AI corridor and a wrong spawn yaw. STRIPB.DAT is the
    # forward track reverse-numbered, so build_routes(stripb) produces headings
    # that point along the reverse driving direction. Only emitted when the track
    # ships reverse geometry — forward-only tracks (no stripb) get no reverse data,
    # so td5_asset_track_has_reverse() correctly keeps them forward-only.
    if stripb:
        left_rev, right_rev = build_routes(stripb, circuit=bool(circuit))
        write("LEFTB.TRK", left_rev)
        write("RIGHTB.TRK", right_rev)

    models_note = "omitted -> placeholder geometry"
    if with_models:
        td6_models = read_zip_entry(zp, "models.dat")
        if td6_models:
            # Texture pages whose meshes are camera-facing billboards (TD6 trees
            # are flat quads — flag them so TD5 yaws them to face the camera).
            try:
                _dir = _parse_textures_dir(_td6_texture_zip(td6_level))
                tree_pages = _billboard_pages(_dir)
                banner_pages = _banner_pages(_dir)
            except Exception:
                tree_pages = frozenset()
                banner_pages = frozenset()
            # Route tangent (on the FORWARD strip) used to cull double-sided
            # checkpoint/start/finish banner rear faces (port renderer can't).
            route_fwd = make_route_fwd(strip, bool(circuit))
            td5_models, mstats = convert_models(td6_models, verbose=verbose,
                                                tree_pages=tree_pages,
                                                banner_pages=banner_pages,
                                                route_fwd=route_fwd)
            if verbose and banner_pages:
                print(f"  banners: {len(banner_pages)} checkpoint/start/finish "
                      f"texture pages -> rear-face culled")
            write("MODELS.DAT", td5_models)
            if verbose and tree_pages:
                print(f"  billboards: {len(tree_pages)} tree texture pages flagged "
                      f"camera-facing")
            models_note = f"de-indexed {mstats['deindexed']}/{mstats['meshes']} 0x04 meshes"
            # textures.dat: one page per textures.dir index referenced by the meshes
            up = mstats.get("used_pages") or []
            page_count = (max(up) + 1) if up else 0
            if page_count > 0:
                try:
                    tex = build_textures_dat(td6_level, page_count, verbose=verbose)
                    if tex:
                        write("TEXTURES.DAT", tex)
                        models_note += " + textures.dat"
                    nlt = build_loose_textures(td6_level, page_count, od, verbose=verbose)
                    if nlt:
                        models_note += f" + {nlt} native PNGs"
                except ImportError:
                    print("  textures.dat: PIL/Pillow not available — skipping (pip install Pillow)")
        else:
            models_note = "no models.dat in source"

    # Sky background (TD6 per-level sky tiles -> FORWSKY/BACKSKY.png)
    try:
        build_sky(td6_level, od, verbose=verbose)
    except ImportError:
        pass

    h = strip_header(strip)
    print(f"Converted TD6 level{td6_level:03d} -> {od}")
    print(f"  strip: spans(main)={h['span_cnt']} phys_spans={h['phys_spans']} "
          f"verts={h['vtx_cnt']} jump@0x14={h['jump_count_at_0x14']}")
    print(f"  levelinf: circuit={circuit}")
    print(f"  models: {models_note}")
    print(f"  drive with: --OverrideTrackZip={td5_level} --AutoRace=1 --SkipIntro=1 --DefaultTrack=0")
    return True


# ------------------------------------------------------------------- report mode
def mesh_render_type_histogram(models: bytes):
    """Walk the models.dat (size,offset) directory and histogram the first byte
    (render_type) of each mesh header. Quick proof that TD6 meshes are 0x04."""
    if not models or len(models) < 8:
        return {}
    d0, d1 = struct.unpack_from("<2I", models, 0)
    # format A: count header at 0, table at 4; else format B table at 0
    if 0 < d0 < 10000 and d1 == 4 + d0 * 8:
        count, tbl = d0, 4
    elif d1 and (d1 & 7) == 0:
        count, tbl = d1 // 8, 0
    else:
        return {"_undetermined": (d0, d1)}
    hist = {}
    for i in range(min(count, 4096)):
        off = tbl + i * 8
        if off + 8 > len(models):
            break
        f0, f1 = struct.unpack_from("<2I", models, off)
        block = f0 if (f0 >= tbl + count * 8 and f0 < len(models)) else f1
        if block + 4 > len(models):
            continue
        sub = struct.unpack_from("<I", models, block)[0]
        if not (1 <= sub <= 256):
            continue
        for j in range(sub):
            so = block + 4 + j * 4
            if so + 4 > len(models):
                break
            moff = struct.unpack_from("<I", models, so)[0]
            mabs = block + moff if block + moff + 1 <= len(models) else moff
            if 0 <= mabs < len(models):
                rt = models[mabs]
                hist[rt] = hist.get(rt, 0) + 1
    return hist


def cmd_report(td6_level: int):
    zp = td6_level_zip(td6_level)
    print(f"=== TD6 level{td6_level:03d} report ({zp}) ===")
    if not os.path.exists(zp):
        print("  (missing)"); return
    with zipfile.ZipFile(zp) as z:
        names = sorted(z.namelist())
    print("  entries:", ", ".join(names))
    strip = read_zip_entry(zp, "strip.dat")
    if strip:
        print("  strip.dat header:", strip_header(strip))
    models = read_zip_entry(zp, "models.dat")
    if models:
        hist = mesh_render_type_histogram(models)
        print(f"  models.dat: {len(models)} bytes  render_type histogram:",
              {hex(k) if isinstance(k, int) else k: v for k, v in hist.items()})


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Migrate TD6 tracks into TD5RE (Strategy A).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("convert", help="Milestone A: emit a drivable TD5 loose-file level")
    a.add_argument("td6_level", type=int, help="TD6 LEVELS/level<NNN>.zip number (e.g. 10)")
    a.add_argument("td5_level", type=int, help="output TD5 level number (use an unused <=40, e.g. 7)")
    a.add_argument("--circuit", type=int, default=1, choices=[0, 1],
                   help="LEVELINF circuit flag: 1=circuit/laps (default), 0=point-to-point")
    a.add_argument("--no-models", action="store_true",
                   help="Milestone A only: skip models.dat de-index (placeholder geometry)")
    a.add_argument("--no-shoulders", dest="shoulders", action="store_false",
                   help="don't synthesize grass shoulders on road-span edges")
    a.add_argument("--urban", action="store_true",
                   help="URBAN/city track: treat ALL spans as road (no grass at "
                        "all). TD6 city tracks reuse surface idx>=16 for road "
                        "(incl. branch roads), which the grass rule mis-slows.")
    a.set_defaults(shoulders=True)

    r = sub.add_parser("report", help="dump format diff / mesh histogram for a TD6 level")
    r.add_argument("td6_level", type=int)

    lt = sub.add_parser("loose-tex", help="emit native-res loose PNGs for an "
                        "already-converted track (no mesh re-de-index)")
    lt.add_argument("td6_level", type=int, help="TD6 level number (texture source)")
    lt.add_argument("td5_level", type=int, help="converted TD5 level number (output dir)")

    args = ap.parse_args()
    if args.cmd == "convert":
        ok = convert_milestone_a(args.td6_level, args.td5_level, args.circuit,
                                 with_models=not args.no_models, shoulders=args.shoulders,
                                 road_only=args.urban)
        sys.exit(0 if ok else 1)
    elif args.cmd == "report":
        cmd_report(args.td6_level)
    elif args.cmd == "loose-tex":
        models = read_zip_entry(td6_level_zip(args.td6_level), "models.dat")
        up = used_pages_of(models) if models else set()
        page_count = (max(up) + 1) if up else 0
        od = out_dir(args.td5_level)
        n = build_loose_textures(args.td6_level, page_count, od) if page_count else 0
        print(f"loose-tex: level{args.td6_level:03d} -> {od} : {n} native PNGs "
              f"(page_count={page_count})")


if __name__ == "__main__":
    main()
