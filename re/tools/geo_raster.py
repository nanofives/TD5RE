"""geo_raster.py -- the self-describing raster container the C side reads.

HEIGHT.R16, CANOPY.R8, COVER.R8 and WATER.R8 all use this one layout, so
td5_tg_world.c needs a single loader rather than four. Everything the loader
needs is in the header: no sidecar, no convention to remember, no way for a
grid to be misread as a different grid.

LAYOUT (little-endian, which is the only endianness this port targets):

    offset size  field
    0      8     magic, "TD5GEOR1"
    8      4     int32   kind          1 = int16 samples, 2 = uint8 samples
    12     4     int32   width         cells
    16     4     int32   height        cells
    20     8     double  origin_x      world units, centre of cell (0, 0)
    28     8     double  origin_z      world units, centre of cell (0, 0)
    36     8     double  cell          world units per cell side
    44     8     double  scale         value = raw * scale + bias
    52     8     double  bias          world units for HEIGHT, class id for COVER
    60     4     int32   nodata_raw    raw value meaning "no sample"
    64     8     double  rotation_rad  the frame this grid is expressed in
    72     -     samples, row-major, z increasing with row index

THE ROTATION FIELD EXISTS BECAUSE ITS ABSENCE ALREADY CAUSED A BUG. The route is
rotated so its start tangent lands on +X, and the rasters must be built in THAT
frame. The first version stored no rotation, geo_fetch built the grid unrotated,
and the terrain silently did not correspond to the road anywhere -- no exception,
no warning, just wrong. Samples are axis-aligned in the stored frame, so the C
side still needs no trigonometry; this field is here to be ASSERTED against the
route's own rotation.

WHY A HEADER AND NOT A CONVENTION. The cell size must equal TG_WORLD_CELL and
the origin must agree with the route's frame, and if either silently disagreed
the terrain would be offset from the road with no error anywhere -- the class of
bug that costs a day. Writing them down lets the loader assert them.
"""
from __future__ import annotations

import os
import struct

import numpy as np

MAGIC = b"TD5GEOR1"
KIND_I16 = 1
KIND_U8 = 2
_HEADER = "<8siii ddd dd i d"
HEADER_SIZE = 72


class Raster:
    """A grid of samples plus the frame it lives in."""

    def __init__(self, data: np.ndarray, origin_x: float, origin_z: float,
                 cell: float, scale: float = 1.0, bias: float = 0.0,
                 nodata_raw: int = -32768, rotation_rad: float = 0.0):
        self.data = data
        self.origin_x = float(origin_x)
        self.origin_z = float(origin_z)
        self.cell = float(cell)
        self.scale = float(scale)
        self.bias = float(bias)
        self.nodata_raw = int(nodata_raw)
        self.rotation_rad = float(rotation_rad)

    @property
    def kind(self) -> int:
        if self.data.dtype == np.int16:
            return KIND_I16
        if self.data.dtype == np.uint8:
            return KIND_U8
        raise TypeError("unsupported raster dtype %r" % self.data.dtype)

    def value(self, ix: int, iz: int) -> float:
        return self.data[iz, ix] * self.scale + self.bias

    def stats(self) -> dict:
        d = self.data
        valid = d != self.nodata_raw if self.kind == KIND_I16 else np.ones_like(d, bool)
        if not valid.any():
            return {"width": int(d.shape[1]), "height": int(d.shape[0]),
                    "valid": 0}
        v = d[valid].astype(np.float64) * self.scale + self.bias
        return {
            "width": int(d.shape[1]),
            "height": int(d.shape[0]),
            "valid": int(valid.sum()),
            "nodata": int((~valid).sum()),
            "min": float(v.min()),
            "max": float(v.max()),
            "mean": float(v.mean()),
        }

    def write(self, path: str) -> None:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        h, w = self.data.shape
        head = struct.pack(_HEADER, MAGIC, self.kind, w, h,
                           self.origin_x, self.origin_z, self.cell,
                           self.scale, self.bias, self.nodata_raw,
                           self.rotation_rad)
        assert len(head) == HEADER_SIZE, len(head)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(head)
            f.write(np.ascontiguousarray(self.data).tobytes())
        os.replace(tmp, path)

    @staticmethod
    def read(path: str) -> "Raster":
        with open(path, "rb") as f:
            head = f.read(HEADER_SIZE)
            (magic, kind, w, h, ox, oz, cell, scale, bias,
             nodata, rot) = struct.unpack(_HEADER, head)
            if magic != MAGIC:
                raise ValueError("%s: bad magic %r" % (path, magic))
            dt = np.int16 if kind == KIND_I16 else np.uint8
            data = np.frombuffer(f.read(), dtype=dt).reshape(h, w)
        return Raster(data.copy(), ox, oz, cell, scale, bias, nodata, rot)
